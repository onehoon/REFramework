#include "mods/re4_xess/RE4XeSS.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>
#include <bcrypt.h>

#include <sdk/GameIdentity.hpp>
#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/SceneManager.hpp>
#include <spdlog/spdlog.h>
#include <safetyhook.hpp>
#include <utility/Address.hpp>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/VtableHook.hpp>

#include "mods/REFrameworkConfig.hpp"
#include "REFramework.hpp"
#include "compatibility/xefg/XeFGCompatibility.hpp"
#include "mods/re4_xess/RE4XeSSSceneViewDecision.hpp"
#include "mods/re4_xess/RE4XeSSLoadEligibility.hpp"

namespace {

using UpscalingMode = RE4XeSS::UpscalingMode;

constexpr std::string_view UPSCALING_MODE_CONFIG_KEY{ "RE4XeSS_UpscalingMode" };
constexpr std::string_view HANDOFF_PROVENANCE_CONFIG_KEY{ "RE4XeSS_HandoffProvenance" };
constexpr char INHIBIT_BIT_BACKING_FIELD_NAME[]{ "<InhibitBit>k__BackingField" };

struct RenderContextTargetSnapshot {
    uintptr_t context{};
    uintptr_t target_state{};
    uintptr_t target_resource{};
    bool valid{};
};

RenderContextTargetSnapshot snapshot_render_context_target(void* render_context) noexcept {
    RenderContextTargetSnapshot snapshot{};
    snapshot.context = reinterpret_cast<uintptr_t>(render_context);
    if (render_context == nullptr) {
        return snapshot;
    }

    // Diagnostic-only observation at the existing Overlay::draw hook boundary.
    // The RenderContext target is transient and does not prove GPU submission.
    const auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
    const auto* target_state = context->get_render_target();
    snapshot.target_state = reinterpret_cast<uintptr_t>(target_state);
    if (target_state != nullptr) {
        snapshot.target_resource = reinterpret_cast<uintptr_t>(target_state->get_native_resource_d3d12());
    }
    snapshot.valid = true;
    return snapshot;
}

constexpr std::array<const char*, 8> UPSCALING_MODE_LABELS{
    "Off",
    "Native AA",
    "Ultra Quality Plus",
    "Ultra Quality",
    "Quality",
    "Balanced",
    "Performance",
    "Ultra Performance",
};

std::optional<std::string> reframework_module_sha256() {
    const auto module = REFramework::get_reframework_module();
    if (module == nullptr) {
        return std::nullopt;
    }

    std::array<wchar_t, 32768> module_path{};
    const auto path_length = GetModuleFileNameW(module, module_path.data(),
        static_cast<DWORD>(module_path.size()));
    if (path_length == 0 || path_length >= module_path.size()) {
        return std::nullopt;
    }

    struct Sha256Api {
        using OpenAlgorithmProvider = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
        using GetProperty = NTSTATUS(WINAPI*)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG*, ULONG);
        using CreateHash = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG,
            PUCHAR, ULONG, ULONG);
        using HashData = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
        using FinishHash = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
        using DestroyHash = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE);
        using CloseAlgorithmProvider = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);

        HMODULE module{};
        OpenAlgorithmProvider open_algorithm_provider{};
        GetProperty get_property{};
        CreateHash create_hash{};
        HashData hash_data{};
        FinishHash finish_hash{};
        DestroyHash destroy_hash{};
        CloseAlgorithmProvider close_algorithm_provider{};
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};

        ~Sha256Api() {
            if (hash != nullptr && destroy_hash != nullptr) {
                destroy_hash(hash);
            }
            if (algorithm != nullptr && close_algorithm_provider != nullptr) {
                close_algorithm_provider(algorithm, 0);
            }
            if (module != nullptr) {
                FreeLibrary(module);
            }
        }
    } api;

    api.module = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (api.module == nullptr) {
        return std::nullopt;
    }
    api.open_algorithm_provider = reinterpret_cast<Sha256Api::OpenAlgorithmProvider>(
        GetProcAddress(api.module, "BCryptOpenAlgorithmProvider"));
    api.get_property = reinterpret_cast<Sha256Api::GetProperty>(
        GetProcAddress(api.module, "BCryptGetProperty"));
    api.create_hash = reinterpret_cast<Sha256Api::CreateHash>(
        GetProcAddress(api.module, "BCryptCreateHash"));
    api.hash_data = reinterpret_cast<Sha256Api::HashData>(GetProcAddress(api.module, "BCryptHashData"));
    api.finish_hash = reinterpret_cast<Sha256Api::FinishHash>(GetProcAddress(api.module, "BCryptFinishHash"));
    api.destroy_hash = reinterpret_cast<Sha256Api::DestroyHash>(GetProcAddress(api.module, "BCryptDestroyHash"));
    api.close_algorithm_provider = reinterpret_cast<Sha256Api::CloseAlgorithmProvider>(
        GetProcAddress(api.module, "BCryptCloseAlgorithmProvider"));
    if (api.open_algorithm_provider == nullptr || api.get_property == nullptr || api.create_hash == nullptr ||
        api.hash_data == nullptr || api.finish_hash == nullptr || api.destroy_hash == nullptr ||
        api.close_algorithm_provider == nullptr ||
        api.open_algorithm_provider(&api.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return std::nullopt;
    }

    ULONG object_length{};
    ULONG hash_length{};
    ULONG result_length{};
    if (api.get_property(api.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length),
            sizeof(object_length), &result_length, 0) < 0 ||
        api.get_property(api.algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_length),
            sizeof(hash_length), &result_length, 0) < 0 || hash_length != 32) {
        return std::nullopt;
    }

    std::vector<UCHAR> hash_object(object_length);
    if (api.create_hash(api.algorithm, &api.hash, hash_object.data(), object_length, nullptr, 0, 0) < 0) {
        return std::nullopt;
    }

    const auto file = CreateFileW(module_path.data(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    struct FileCloser {
        HANDLE handle;
        ~FileCloser() { CloseHandle(handle); }
    } file_closer{ file };

    std::array<UCHAR, 64 * 1024> buffer{};
    for (;;) {
        DWORD bytes_read{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
            return std::nullopt;
        }
        if (bytes_read == 0) {
            break;
        }
        if (api.hash_data(api.hash, buffer.data(), bytes_read, 0) < 0) {
            return std::nullopt;
        }
    }

    std::array<UCHAR, 32> digest{};
    if (api.finish_hash(api.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        return std::nullopt;
    }
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(HEX[byte >> 4]);
        result.push_back(HEX[byte & 0x0f]);
    }
    return result;
}

void log_re4_xess_runtime_identity() {
    static std::once_flag logged;
    std::call_once(logged, [] {
        if (const auto digest = reframework_module_sha256(); digest.has_value()) {
            spdlog::info("[RE4XeSS][RuntimeIdentity] refDllSha256={}", *digest);
        } else {
            spdlog::warn("[RE4XeSS][RuntimeIdentity] refDllSha256=unavailable");
        }
    });
}

const char* lifetime_trace_kind_name(RE4XeSSLifetimeTrace::Kind kind) noexcept {
    using Kind = RE4XeSSLifetimeTrace::Kind;
    switch (kind) {
    case Kind::SceneFrame: return "scene-frame";
    case Kind::SceneViewSize: return "scene-view-size";
    case Kind::PreOverlay: return "pre-overlay";
    case Kind::Submit: return "submit";
    case Kind::OutputInstall: return "output-install";
    case Kind::OverlayRenderContext: return "overlay-render-context";
    case Kind::OutputRestore: return "output-restore";
    case Kind::ModeTransition: return "mode-transition";
    case Kind::PostOverlayObservation: return "post-overlay-observation";
    case Kind::Present: return "present";
    case Kind::PostPresentCallback: return "post-present-callback";
    case Kind::Marker: return "marker";
    case Kind::WriterFenceComplete: return "writer-fence-complete";
    case Kind::DownstreamFenceComplete: return "downstream-fence-complete";
    case Kind::LoadAdmission: return "load-admission";
    case Kind::Skip: return "skip";
    case Kind::Count: break;
    }
    return "unknown";
}

const char* load_state_callback_name(uint32_t callback_kind) noexcept {
    switch (callback_kind) {
    case 0: return "scene-view-size";
    case 1: return "camera-projection";
    case 2: return "scene-layer-update";
    case 3: return "pre-overlay";
    default: return "unknown";
    }
}

const char* lifetime_trace_phase_name(RE4XeSSLifetimeTrace::CapturePhase phase) noexcept {
    return phase == RE4XeSSLifetimeTrace::CapturePhase::ActiveXeSS ? "active-xess" : "pre-active";
}

const char* lifetime_trace_mapping_name(RE4XeSSLifetimeTrace::MappingState state) noexcept {
    using MappingState = RE4XeSSLifetimeTrace::MappingState;
    switch (state) {
    case MappingState::Unknown: return "unknown";
    case MappingState::InferredCandidate: return "inferred-candidate";
    case MappingState::Proven: return "proven";
    }
    return "unknown";
}

const char* lifetime_trace_render_context_stage_name(
    RE4XeSSLifetimeTrace::RenderContextStage stage) noexcept {
    using Stage = RE4XeSSLifetimeTrace::RenderContextStage;
    switch (stage) {
    case Stage::BeforeOriginalOverlayDraw: return "pre-original-overlay-draw";
    case Stage::PostOverlayCallbackOriginalStatusUnknown: return "post-overlay-callback-original-status-unknown";
    case Stage::None: return "none";
    }
    return "unknown";
}

const char* lifetime_trace_consumer_evidence_name(
    RE4XeSSLifetimeTrace::ConsumerEvidence evidence) noexcept {
    using Evidence = RE4XeSSLifetimeTrace::ConsumerEvidence;
    switch (evidence) {
    case Evidence::Unknown: return "unknown";
    case Evidence::ReaderNotObserved: return "reader-not-observed";
    case Evidence::ReaderObservedSubmitUnproven: return "reader-observed-submit-unproven";
    case Evidence::QueueOrderProven: return "queue-order-proven";
    case Evidence::PresentConsumerProven: return "present-consumer-proven";
    case Evidence::GpuComplete: return "gpu-complete";
    }
    return "unknown";
}

const char* lifetime_trace_dump_window_name(RE4XeSSLifetimeTrace::DumpWindow window) noexcept {
    using DumpWindow = RE4XeSSLifetimeTrace::DumpWindow;
    switch (window) {
    case DumpWindow::PreActive: return "pre-active";
    case DumpWindow::ActiveMilestone: return "active-milestone";
    case DumpWindow::Anomaly: return "anomaly";
    case DumpWindow::ModeTransition: return "mode-transition";
    case DumpWindow::Periodic: return "periodic";
    case DumpWindow::Count: break;
    }
    return "unknown";
}

void dump_re4_xess_lifetime_trace(
    std::string_view reason,
    size_t event_count,
    RE4XeSSLifetimeTrace::DumpWindow window) {
    auto& trace = RE4XeSSLifetimeTrace::instance();
    if (!trace.enabled()) {
        return;
    }

    RE4XeSSLifetimeTrace::DumpReservation reservation{};
    if (!trace.reserve_dump(window, event_count, reservation)) {
        return;
    }

    const auto summary = trace.summary();
    const auto events = trace.recent(reservation.event_count);
    const auto bounded_reason = reason.substr(0, 64);
    spdlog::info("[RE4XeSS][LifetimeTrace] window reason={} class={} window={} phase={} selected={} retained={} preActiveEvents={} activeEvents={} preActiveOverwritten={} activeOverwritten={} activeInstalls={} activeUnmarked={} activeUnmarkedHighWater={} activeMarkedGpuIncomplete={} activeExecuteSuccess={} activeMarkerQueued={} activeResetHistory={} activeContinuous={} activeSkips={} activeMarkerPendingSkips={} activeBridgeBusySkips={} activeTemporalGateSkips={} activeSceneViewSizeSamples={} sceneViewSameIdentityChanges={} sceneViewCrossIdentityVariations={} sceneViewDrops={} sceneViewCacheReplacements={} sceneViewKeyReentries={} sceneViewEvictedKeyHistoryOverwrites={} activeSceneViewSameIdentityChanges={} activeSceneViewCrossIdentityVariations={} activeSceneViewDrops={} activeSceneViewCacheReplacements={} activeSceneViewKeyReentries={} activeSceneViewEvictedKeyHistoryOverwrites={} activeSceneJitterFrames={} activeRenderContextSamples={} activeRenderContextStateMatches={} activeRenderContextResourceMatches={} activeRenderContextOverlayMainStateMatches={} activeRenderContextOverlayMainResourceMatches={} activeMapUnknown={} activeMapCandidate={} activeMapProven={} dumpRequests={} dumpEmitted={} dumpSuppressed={} eventLinesReserved={} eventLinesEmitted={} bytesReserved={} allEvents={}",
        bounded_reason,
        lifetime_trace_dump_window_name(window),
        reservation.index,
        reservation.active_phase ? "active-xess" : "pre-active",
        events.size(),
        static_cast<unsigned long long>(summary.retained_events),
        static_cast<unsigned long long>(summary.pre_active_events),
        static_cast<unsigned long long>(summary.active_events),
        static_cast<unsigned long long>(summary.pre_active_overwritten_events),
        static_cast<unsigned long long>(summary.active_overwritten_events),
        static_cast<unsigned long long>(summary.active_output_installs),
        static_cast<unsigned long long>(summary.installed_unmarked),
        static_cast<unsigned long long>(summary.installed_unmarked_high_water),
        static_cast<unsigned long long>(summary.marked_gpu_incomplete),
        static_cast<unsigned long long>(summary.active_successful_submissions),
        static_cast<unsigned long long>(summary.active_marker_queued),
        static_cast<unsigned long long>(summary.active_reset_history_submits),
        static_cast<unsigned long long>(summary.active_continuous_submits),
        static_cast<unsigned long long>(summary.active_skipped_submits),
        static_cast<unsigned long long>(summary.active_marker_pending_skips),
        static_cast<unsigned long long>(summary.active_bridge_busy_skips),
        static_cast<unsigned long long>(summary.active_temporal_gate_skips),
        static_cast<unsigned long long>(summary.active_event_counts[static_cast<size_t>(RE4XeSSLifetimeTrace::Kind::SceneViewSize)]),
        static_cast<unsigned long long>(summary.scene_view_same_identity_changes),
        static_cast<unsigned long long>(summary.scene_view_cross_identity_variations),
        static_cast<unsigned long long>(summary.scene_view_size_observation_drops),
        static_cast<unsigned long long>(summary.scene_view_frame_cache_replacements),
        static_cast<unsigned long long>(summary.scene_view_frame_key_reentries),
        static_cast<unsigned long long>(summary.scene_view_evicted_key_history_overwrites),
        static_cast<unsigned long long>(summary.active_scene_view_same_identity_changes),
        static_cast<unsigned long long>(summary.active_scene_view_cross_identity_variations),
        static_cast<unsigned long long>(summary.active_scene_view_size_observation_drops),
        static_cast<unsigned long long>(summary.active_scene_view_frame_cache_replacements),
        static_cast<unsigned long long>(summary.active_scene_view_frame_key_reentries),
        static_cast<unsigned long long>(summary.active_scene_view_evicted_key_history_overwrites),
        static_cast<unsigned long long>(summary.active_event_counts[static_cast<size_t>(RE4XeSSLifetimeTrace::Kind::SceneFrame)]),
        static_cast<unsigned long long>(summary.active_render_context_samples),
        static_cast<unsigned long long>(summary.active_render_context_state_matches),
        static_cast<unsigned long long>(summary.active_render_context_resource_matches),
        static_cast<unsigned long long>(summary.active_render_context_overlay_main_state_matches),
        static_cast<unsigned long long>(summary.active_render_context_overlay_main_resource_matches),
        static_cast<unsigned long long>(summary.active_mapping_unknown),
        static_cast<unsigned long long>(summary.active_mapping_inferred_candidates),
        static_cast<unsigned long long>(summary.active_mapping_proven),
        static_cast<unsigned long long>(summary.dump_requests),
        static_cast<unsigned long long>(summary.dump_windows_emitted),
        static_cast<unsigned long long>(summary.dump_windows_suppressed),
        static_cast<unsigned long long>(summary.dump_event_lines_reserved),
        static_cast<unsigned long long>(summary.dump_event_lines_emitted),
        static_cast<unsigned long long>(summary.dump_bytes_reserved),
        static_cast<unsigned long long>(summary.total_events));

    for (const auto& event : events) {
        spdlog::info("[RE4XeSS][LifetimeTrace] seq={} us={} phase={} kind={} trace={} install={} outputUseToken={} consumerEvidence={} frame={} frameValid={} sceneView=0x{:x} sceneViewSizeNative={}x{} sceneViewSizeEffective={}x{} temporalActive={} sceneViewOverride={} sceneViewSameIdentityChanged={} sceneViewCrossIdentityVaried={} sceneViewFrameKeyReentered={} xessInput={}x{} display={}x{} inputResolutionValid={} jitterApplied={} jitterPixels=({}, {}) jitterPhase={}/{} sceneOrd={} callbackOrd={} overlapEpoch={} submit={} present={} relatedPresent={} outputGen={} controlGen={} deviceGen={} bridgeSlot={} writerFence={} downstreamFence={} cachedCompleted={} cachedValid={} actualCompleted={} actualValid={} writerTxn={} writerInvocation={} writerSeq={} clearSeq={}->{} replacementSeq={}->{} writerRva=0x{:x} presentTimes={}::{}/{} output=0x{:x} targetState=0x{:x} renderContext=0x{:x} contextStage={} contextTargetState=0x{:x} contextTargetResource=0x{:x} contextSampleValid={} contextStateMatch={} contextResourceMatch={} overlayMainState=0x{:x} overlayMainResource=0x{:x} contextOverlayMainStateMatch={} contextOverlayMainResourceMatch={} overlay=0x{:x} swapchain=0x{:x} device=0x{:x} queue=0x{:x} queueType={} queueTypeValid={} tid={} presentTids={}->{} presentSource={} present1={} presentReturned={} callbacksSuppressed={} originalSkipped={} result=0x{:08x} apiOk={} queueSubmitted={} markerQueued={} resetHistory={} mappingState={} mappingAmbiguous={} reason={}",
            static_cast<unsigned long long>(event.sequence),
            static_cast<unsigned long long>(event.timestamp_us),
            lifetime_trace_phase_name(event.capture_phase),
            lifetime_trace_kind_name(event.kind),
            static_cast<unsigned long long>(event.trace_id),
            static_cast<unsigned long long>(event.install_id),
            static_cast<unsigned long long>(event.output_use_token),
            lifetime_trace_consumer_evidence_name(event.consumer_evidence),
            static_cast<unsigned long long>(event.frame_id),
            event.frame_valid,
            event.scene_view,
            event.scene_view_native_width,
            event.scene_view_native_height,
            event.scene_view_effective_width,
            event.scene_view_effective_height,
            event.temporal_active,
            event.scene_view_override_applied,
            event.scene_view_same_identity_changed,
            event.scene_view_cross_identity_varied,
            event.scene_view_frame_key_reentered,
            event.input_width,
            event.input_height,
            event.display_width,
            event.display_height,
            event.input_resolution_valid,
            event.jitter_applied,
            event.jitter_x_pixels,
            event.jitter_y_pixels,
            event.jitter_sample_index,
            event.jitter_phase_count,
            static_cast<unsigned long long>(event.scene_ordinal),
            static_cast<unsigned long long>(event.callback_ordinal),
            static_cast<unsigned long long>(event.overlap_epoch),
            static_cast<unsigned long long>(event.submit_ordinal),
            static_cast<unsigned long long>(event.present_ordinal),
            static_cast<unsigned long long>(event.related_present_ordinal),
            static_cast<unsigned long long>(event.output_generation),
            static_cast<unsigned long long>(event.control_generation),
            static_cast<unsigned long long>(event.device_reset_generation),
            event.bridge_slot,
            static_cast<unsigned long long>(event.writer_fence_value),
            static_cast<unsigned long long>(event.downstream_fence_value),
            static_cast<unsigned long long>(event.cached_completed_value),
            event.cached_completed_value_valid,
            static_cast<unsigned long long>(event.actual_completed_value),
            event.actual_completed_value_valid,
            event.writer_transaction_confirmed,
            static_cast<unsigned long long>(event.writer_invocation_id),
            static_cast<unsigned long long>(event.writer_sequence),
            static_cast<unsigned long long>(event.writer_clear_pre_sequence),
            static_cast<unsigned long long>(event.writer_clear_post_sequence),
            static_cast<unsigned long long>(event.writer_replacement_pre_sequence),
            static_cast<unsigned long long>(event.writer_replacement_post_sequence),
            event.writer_method_rva,
            static_cast<unsigned long long>(event.present_entry_time_us),
            static_cast<unsigned long long>(event.original_present_enter_time_us),
            static_cast<unsigned long long>(event.original_present_return_time_us),
            event.output_resource,
            event.target_state,
            event.render_context,
            lifetime_trace_render_context_stage_name(event.render_context_stage),
            event.render_context_target_state,
            event.render_context_target_resource,
            event.render_context_sample_valid,
            event.render_context_target_state_matches_output,
            event.render_context_target_resource_matches_output,
            event.overlay_main_target_state,
            event.overlay_main_target_resource,
            event.render_context_target_state_matches_overlay_main,
            event.render_context_target_resource_matches_overlay_main,
            event.overlay,
            event.swapchain,
            event.device,
            event.queue,
            event.command_queue_type,
            event.command_queue_type_valid,
            event.thread_id,
            event.present_entry_thread_id,
            event.present_return_thread_id,
            event.present_source,
            event.present1,
            event.present_returned,
            event.present_callbacks_suppressed,
            event.original_present_skipped,
            static_cast<uint32_t>(event.result),
            event.api_succeeded,
            event.queue_submitted,
            event.marker_queued,
            event.reset_history,
            lifetime_trace_mapping_name(event.mapping_state),
            event.mapping_ambiguous,
            event.reason.data());
    }
    trace.note_dump_emitted(reservation, events.size());
}

void log_re4_xess_lifetime_summary() {
    auto& trace = RE4XeSSLifetimeTrace::instance();
    if (!trace.claim_final_summary()) {
        return;
    }

    const auto summary = trace.summary();
    const auto kind_count = [&](RE4XeSSLifetimeTrace::Kind kind, bool active) {
        const auto index = static_cast<size_t>(kind);
        return active ? summary.active_event_counts[index] : summary.pre_active_event_counts[index];
    };
    using Kind = RE4XeSSLifetimeTrace::Kind;
    spdlog::info("[RE4XeSS][LifetimeTrace] lifecycle-summary activeStarted={} recorded={} retained={} overwritten={} preActiveEvents={} preScene={} preOverlay={} preSkip={} activeEvents={} activeScene={} activeSceneViewSizeSamples={} sceneViewSameIdentityChanges={} sceneViewCrossIdentityVariations={} sceneViewDrops={} sceneViewCacheReplacements={} sceneViewKeyReentries={} sceneViewEvictedKeyHistoryOverwrites={} activeSceneViewSameIdentityChanges={} activeSceneViewCrossIdentityVariations={} activeSceneViewDrops={} activeSceneViewCacheReplacements={} activeSceneViewKeyReentries={} activeSceneViewEvictedKeyHistoryOverwrites={} activeJitterFrames={} activePreOverlay={} activeSubmit={} activeInstall={} activeOverlayContextSamples={} activeOverlayContextStateMatches={} activeOverlayContextResourceMatches={} activeOverlayContextOverlayMainStateMatches={} activeOverlayContextOverlayMainResourceMatches={} installedUnmarked={} installedUnmarkedHighWater={} markedGpuIncomplete={} activeRestore={} activeModeChange={} activePostPresent={} activeMarker={} activeSkip={} activeExecuteSuccess={} activeResetHistory={} activeContinuous={} activeMarkerPendingSkip={} activeBridgeBusySkip={} activeTemporalGateSkip={} activeMapUnknown={} activeMapCandidate={} activeMapProven={} preOverwritten={} activeOverwritten={} dumpRequests={} dumpEmitted={} dumpSuppressed={} eventLinesReserved={} eventLinesEmitted={} bytesReserved={} activeGameplayDumpEmitted={}",
        summary.active_capture_started,
        static_cast<unsigned long long>(summary.total_events),
        static_cast<unsigned long long>(summary.retained_events),
        static_cast<unsigned long long>(summary.overwritten_events),
        static_cast<unsigned long long>(summary.pre_active_events),
        static_cast<unsigned long long>(kind_count(Kind::SceneFrame, false)),
        static_cast<unsigned long long>(kind_count(Kind::PreOverlay, false)),
        static_cast<unsigned long long>(kind_count(Kind::Skip, false)),
        static_cast<unsigned long long>(summary.active_events),
        static_cast<unsigned long long>(kind_count(Kind::SceneFrame, true)),
        static_cast<unsigned long long>(kind_count(Kind::SceneViewSize, true)),
        static_cast<unsigned long long>(summary.scene_view_same_identity_changes),
        static_cast<unsigned long long>(summary.scene_view_cross_identity_variations),
        static_cast<unsigned long long>(summary.scene_view_size_observation_drops),
        static_cast<unsigned long long>(summary.scene_view_frame_cache_replacements),
        static_cast<unsigned long long>(summary.scene_view_frame_key_reentries),
        static_cast<unsigned long long>(summary.scene_view_evicted_key_history_overwrites),
        static_cast<unsigned long long>(summary.active_scene_view_same_identity_changes),
        static_cast<unsigned long long>(summary.active_scene_view_cross_identity_variations),
        static_cast<unsigned long long>(summary.active_scene_view_size_observation_drops),
        static_cast<unsigned long long>(summary.active_scene_view_frame_cache_replacements),
        static_cast<unsigned long long>(summary.active_scene_view_frame_key_reentries),
        static_cast<unsigned long long>(summary.active_scene_view_evicted_key_history_overwrites),
        static_cast<unsigned long long>(kind_count(Kind::SceneFrame, true)),
        static_cast<unsigned long long>(kind_count(Kind::PreOverlay, true)),
        static_cast<unsigned long long>(kind_count(Kind::Submit, true)),
        static_cast<unsigned long long>(kind_count(Kind::OutputInstall, true)),
        static_cast<unsigned long long>(summary.active_render_context_samples),
        static_cast<unsigned long long>(summary.active_render_context_state_matches),
        static_cast<unsigned long long>(summary.active_render_context_resource_matches),
        static_cast<unsigned long long>(summary.active_render_context_overlay_main_state_matches),
        static_cast<unsigned long long>(summary.active_render_context_overlay_main_resource_matches),
        static_cast<unsigned long long>(summary.installed_unmarked),
        static_cast<unsigned long long>(summary.installed_unmarked_high_water),
        static_cast<unsigned long long>(summary.marked_gpu_incomplete),
        static_cast<unsigned long long>(kind_count(Kind::OutputRestore, true)),
        static_cast<unsigned long long>(kind_count(Kind::ModeTransition, true)),
        static_cast<unsigned long long>(kind_count(Kind::PostPresentCallback, true)),
        static_cast<unsigned long long>(kind_count(Kind::Marker, true)),
        static_cast<unsigned long long>(kind_count(Kind::Skip, true)),
        static_cast<unsigned long long>(summary.active_successful_submissions),
        static_cast<unsigned long long>(summary.active_reset_history_submits),
        static_cast<unsigned long long>(summary.active_continuous_submits),
        static_cast<unsigned long long>(summary.active_marker_pending_skips),
        static_cast<unsigned long long>(summary.active_bridge_busy_skips),
        static_cast<unsigned long long>(summary.active_temporal_gate_skips),
        static_cast<unsigned long long>(summary.active_mapping_unknown),
        static_cast<unsigned long long>(summary.active_mapping_inferred_candidates),
        static_cast<unsigned long long>(summary.active_mapping_proven),
        static_cast<unsigned long long>(summary.pre_active_overwritten_events),
        static_cast<unsigned long long>(summary.active_overwritten_events),
        static_cast<unsigned long long>(summary.dump_requests),
        static_cast<unsigned long long>(summary.dump_windows_emitted),
        static_cast<unsigned long long>(summary.dump_windows_suppressed),
        static_cast<unsigned long long>(summary.dump_event_lines_reserved),
        static_cast<unsigned long long>(summary.dump_event_lines_emitted),
        static_cast<unsigned long long>(summary.dump_bytes_reserved),
        static_cast<unsigned long long>(summary.active_gameplay_dump_emitted));
}

std::optional<UpscalingMode> mode_from_config_token(std::string_view token) {
    if (token == "off") return UpscalingMode::Off;
    if (token == "native_aa") return UpscalingMode::NativeAA;
    if (token == "ultra_quality_plus") return UpscalingMode::UltraQualityPlus;
    if (token == "ultra_quality") return UpscalingMode::UltraQuality;
    if (token == "quality") return UpscalingMode::Quality;
    if (token == "balanced") return UpscalingMode::Balanced;
    if (token == "performance") return UpscalingMode::Performance;
    if (token == "ultra_performance") return UpscalingMode::UltraPerformance;
    return std::nullopt;
}

std::string_view mode_to_config_token(UpscalingMode mode) {
    switch (mode) {
    case UpscalingMode::Off: return "off";
    case UpscalingMode::NativeAA: return "native_aa";
    case UpscalingMode::UltraQualityPlus: return "ultra_quality_plus";
    case UpscalingMode::UltraQuality: return "ultra_quality";
    case UpscalingMode::Quality: return "quality";
    case UpscalingMode::Balanced: return "balanced";
    case UpscalingMode::Performance: return "performance";
    case UpscalingMode::UltraPerformance: return "ultra_performance";
    }
    return "off";
}

std::string_view mode_to_display_label(UpscalingMode mode) {
    const auto index = static_cast<size_t>(mode);
    return index < UPSCALING_MODE_LABELS.size() ? UPSCALING_MODE_LABELS[index] : UPSCALING_MODE_LABELS[0];
}

std::optional<xess_quality_settings_t> mode_to_quality_setting(UpscalingMode mode) {
    switch (mode) {
    case UpscalingMode::Off: return std::nullopt;
    case UpscalingMode::NativeAA: return XESS_QUALITY_SETTING_AA;
    case UpscalingMode::UltraQualityPlus: return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
    case UpscalingMode::UltraQuality: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    case UpscalingMode::Quality: return XESS_QUALITY_SETTING_QUALITY;
    case UpscalingMode::Balanced: return XESS_QUALITY_SETTING_BALANCED;
    case UpscalingMode::Performance: return XESS_QUALITY_SETTING_PERFORMANCE;
    case UpscalingMode::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    }
    return std::nullopt;
}

struct PreOverlayGateGuard {
    std::atomic_flag& gate;
    bool log_exit{};
    DWORD thread_id{};
    uint64_t overlap_epoch{};

    ~PreOverlayGateGuard() {
        gate.clear(std::memory_order_release);
        if (log_exit) {
            spdlog::info("[RE4XeSS][Coordinator] exit thread={} overlapEpoch={}",
                thread_id,
                static_cast<unsigned long long>(overlap_epoch));
        }
    }
};

float halton(uint32_t sample_index, uint32_t base) {
    float result{};
    float fraction{ 1.0f };
    auto index = sample_index;

    while (index != 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }

    return result;
}

struct GameLoadSnapshot {
    bool pause{};
    uint64_t inhibit{};
};

enum class LoadSnapshotFailure : uint8_t {
    None,
    PauseTypeUnavailable,
    PauseGetterUnavailable,
    PauseGetterInvalidSignature,
    PauseFieldUnavailable,
    SituationTypeUnavailable,
    SituationGetterUnavailable,
    SituationGetterInvalidSignature,
    InhibitFieldUnavailable,
    ThreadContextUnavailable,
    PauseInstanceUnavailable,
    SituationInstanceUnavailable,
    PauseFieldTypeUnavailable,
    PauseFieldTypeMismatch,
    PauseFieldAddressUnavailable,
    PauseFieldValueInvalid,
    InhibitFieldTypeUnavailable,
    InhibitStorageTypeUnavailable,
    InhibitStorageTypeMismatch,
    InhibitStorageWidthInvalid,
    InhibitFieldAddressUnavailable,
    InhibitFieldValueInvalid,
};

std::string_view load_snapshot_failure_name(LoadSnapshotFailure failure) {
    switch (failure) {
    case LoadSnapshotFailure::None: return "Valid";
    case LoadSnapshotFailure::PauseTypeUnavailable: return "PauseTypeUnavailable";
    case LoadSnapshotFailure::PauseGetterUnavailable: return "PauseGetterUnavailable";
    case LoadSnapshotFailure::PauseGetterInvalidSignature: return "PauseGetterInvalidSignature";
    case LoadSnapshotFailure::PauseFieldUnavailable: return "PauseFieldUnavailable";
    case LoadSnapshotFailure::SituationTypeUnavailable: return "SituationTypeUnavailable";
    case LoadSnapshotFailure::SituationGetterUnavailable: return "SituationGetterUnavailable";
    case LoadSnapshotFailure::SituationGetterInvalidSignature: return "SituationGetterInvalidSignature";
    case LoadSnapshotFailure::InhibitFieldUnavailable: return "InhibitFieldUnavailable";
    case LoadSnapshotFailure::ThreadContextUnavailable: return "ThreadContextUnavailable";
    case LoadSnapshotFailure::PauseInstanceUnavailable: return "PauseInstanceUnavailable";
    case LoadSnapshotFailure::SituationInstanceUnavailable: return "SituationInstanceUnavailable";
    case LoadSnapshotFailure::PauseFieldTypeUnavailable: return "PauseFieldTypeUnavailable";
    case LoadSnapshotFailure::PauseFieldTypeMismatch: return "PauseFieldTypeMismatch";
    case LoadSnapshotFailure::PauseFieldAddressUnavailable: return "PauseFieldAddressUnavailable";
    case LoadSnapshotFailure::PauseFieldValueInvalid: return "PauseFieldValueInvalid";
    case LoadSnapshotFailure::InhibitFieldTypeUnavailable: return "InhibitFieldTypeUnavailable";
    case LoadSnapshotFailure::InhibitStorageTypeUnavailable: return "InhibitStorageTypeUnavailable";
    case LoadSnapshotFailure::InhibitStorageTypeMismatch: return "InhibitStorageTypeMismatch";
    case LoadSnapshotFailure::InhibitStorageWidthInvalid: return "InhibitStorageWidthInvalid";
    case LoadSnapshotFailure::InhibitFieldAddressUnavailable: return "InhibitFieldAddressUnavailable";
    case LoadSnapshotFailure::InhibitFieldValueInvalid: return "InhibitFieldValueInvalid";
    }
    return "Unknown";
}

struct LoadFieldObservation {
    sdk::REField* field{};
    sdk::RETypeDefinition* declaring_type{};
    sdk::RETypeDefinition* field_type{};
    sdk::RETypeDefinition* storage_type{};
    const void* address{};
    uint32_t field_size{};
    uint32_t field_value_type_size{};
    uint32_t storage_size{};
    uint32_t storage_value_type_size{};
    uint32_t storage_width{};
    bool is_static{};
    std::optional<uint64_t> raw_value{};
};

struct LoadAccessorObservation {
    sdk::RETypeDefinition* pause_type{};
    sdk::REMethodDefinition* pause_getter{};
    LoadFieldObservation pause_field{};
    sdk::RETypeDefinition* situation_type{};
    sdk::REMethodDefinition* situation_getter{};
    LoadFieldObservation inhibit_field{};
    sdk::RETypeDefinition* situation_runtime_type{};
    void* thread_context{};
    REManagedObject* pause_instance{};
    REManagedObject* situation_instance{};
};

struct GameLoadSnapshotResult {
    std::optional<GameLoadSnapshot> snapshot{};
    LoadSnapshotFailure failure{ LoadSnapshotFailure::None };
    LoadAccessorObservation observation{};
};

struct LoadStateAccessors {
    sdk::RETypeDefinition* pause_type{};
    sdk::REMethodDefinition* pause_instance_getter{};
    sdk::REField* pause_field{};
    sdk::RETypeDefinition* situation_type{};
    sdk::REMethodDefinition* situation_instance_getter{};
    sdk::REField* inhibit_field{};

    void resolve() {
        if (pause_type == nullptr) {
            pause_type = sdk::find_type_definition("chainsaw.SceneLoadZoneManager");
        }
        if (pause_type != nullptr) {
            if (pause_instance_getter == nullptr) pause_instance_getter = pause_type->get_method("get_Instance");
            if (pause_field == nullptr) pause_field = pause_type->get_field("_Pause");
        }

        if (situation_type == nullptr) {
            situation_type = sdk::find_type_definition("chainsaw.GameSituationManager");
        }
        if (situation_type != nullptr) {
            if (situation_instance_getter == nullptr) situation_instance_getter = situation_type->get_method("get_Instance");
            if (inhibit_field == nullptr) inhibit_field = situation_type->get_field(INHIBIT_BIT_BACKING_FIELD_NAME);
        }
    }
};

uint32_t managed_integral_width(const sdk::RETypeDefinition* type) {
    if (type == nullptr) {
        return 0;
    }

    auto* value_type = const_cast<sdk::RETypeDefinition*>(type);
    if (value_type->is_enum()) {
        value_type = value_type->get_underlying_type();
        if (value_type == nullptr) {
            return 0;
        }
    }

    const auto name = value_type->get_full_name();
    if (name == "System.Boolean" || name == "System.SByte" || name == "System.Byte") return 1;
    if (name == "System.Char" || name == "System.Int16" || name == "System.UInt16") return 2;
    if (name == "System.Int32" || name == "System.UInt32") return 4;
    if (name == "System.Int64" || name == "System.UInt64") return 8;
    return 0;
}

bool is_boolean_type(const sdk::RETypeDefinition* type) {
    return type != nullptr && type->get_full_name() == "System.Boolean";
}

void observe_field_metadata(LoadFieldObservation& observation, sdk::REField* field) {
    observation.field = field;
    if (field == nullptr) {
        return;
    }

    observation.declaring_type = field->get_declaring_type();
    observation.field_type = field->get_type();
    observation.is_static = field->is_static();
    if (observation.field_type != nullptr) {
        observation.field_size = observation.field_type->get_size();
        observation.field_value_type_size = observation.field_type->get_valuetype_size();
        observation.storage_type = observation.field_type->is_enum()
            ? observation.field_type->get_underlying_type()
            : observation.field_type;
        if (observation.storage_type != nullptr) {
            observation.storage_size = observation.storage_type->get_size();
            observation.storage_value_type_size = observation.storage_type->get_valuetype_size();
            observation.storage_width = managed_integral_width(observation.storage_type);
        }
    }
}

bool is_integral_type(const sdk::RETypeDefinition* type) {
    return managed_integral_width(type) != 0;
}

struct BoolFieldReadResult {
    std::optional<bool> value{};
    LoadSnapshotFailure failure{ LoadSnapshotFailure::None };
};

struct IntegralFieldReadResult {
    std::optional<uint64_t> value{};
    LoadSnapshotFailure failure{ LoadSnapshotFailure::None };
};

BoolFieldReadResult read_bool_field(LoadFieldObservation& observation, REManagedObject* instance) {
    const auto* type = observation.field_type;
    if (type == nullptr) {
        return { std::nullopt, LoadSnapshotFailure::PauseFieldTypeUnavailable };
    }
    if (!is_boolean_type(type) || managed_integral_width(type) != sizeof(uint8_t)) {
        return { std::nullopt, LoadSnapshotFailure::PauseFieldTypeMismatch };
    }

    observation.address = observation.field->get_data_raw(instance);
    if (observation.address == nullptr) {
        return { std::nullopt, LoadSnapshotFailure::PauseFieldAddressUnavailable };
    }

    uint8_t value{};
    std::memcpy(&value, observation.address, sizeof(value));
    observation.raw_value = value;
    if (value > 1) {
        return { std::nullopt, LoadSnapshotFailure::PauseFieldValueInvalid };
    }

    return { value != 0, LoadSnapshotFailure::None };
}

IntegralFieldReadResult read_integral_field(LoadFieldObservation& observation, REManagedObject* instance) {
    const auto* field_type = observation.field_type;
    if (field_type == nullptr) {
        return { std::nullopt, LoadSnapshotFailure::InhibitFieldTypeUnavailable };
    }
    if (field_type->is_enum() && observation.storage_type == nullptr) {
        return { std::nullopt, LoadSnapshotFailure::InhibitStorageTypeUnavailable };
    }
    if (!is_integral_type(observation.storage_type)) {
        return { std::nullopt, LoadSnapshotFailure::InhibitStorageTypeMismatch };
    }

    const auto width = observation.storage_width;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return { std::nullopt, LoadSnapshotFailure::InhibitStorageWidthInvalid };
    }

    observation.address = observation.field->get_data_raw(instance);
    if (observation.address == nullptr) {
        return { std::nullopt, LoadSnapshotFailure::InhibitFieldAddressUnavailable };
    }

    uint64_t value{};
    std::memcpy(&value, observation.address, width);
    observation.raw_value = value;
    if (is_boolean_type(observation.storage_type) && value > 1) {
        return { std::nullopt, LoadSnapshotFailure::InhibitFieldValueInvalid };
    }
    return { value, LoadSnapshotFailure::None };
}

void append_pointer(std::ostringstream& stream, const void* pointer) {
    if (pointer == nullptr) {
        stream << "null";
        return;
    }
    stream << "0x" << std::hex << reinterpret_cast<uintptr_t>(pointer) << std::dec;
}

void append_type(std::ostringstream& stream, const sdk::RETypeDefinition* type) {
    append_pointer(stream, type);
    stream << "(" << (type != nullptr ? type->get_full_name() : "null");
    if (type != nullptr) {
        stream << ",typeSizeMetadata=" << type->get_size()
               << ",valueTypeSizeMetadata=" << type->get_valuetype_size()
               << ",managedIntegralWidth=" << managed_integral_width(type)
               << ",enum=" << (type->is_enum() ? "true" : "false");
    }
    stream << ")";
}

void append_method(std::ostringstream& stream, std::string_view label, sdk::REMethodDefinition* method) {
    stream << " " << label << "=";
    append_pointer(stream, method);
    if (method == nullptr) {
        return;
    }

    const auto* declaring_type = method->get_declaring_type();
    const auto* return_type = method->get_return_type();
    const auto* method_name = method->get_name();
    stream << "(declaring=" << (declaring_type != nullptr ? declaring_type->get_full_name() : "null")
           << ",name=" << (method_name != nullptr ? method_name : "null")
           << ",static=" << (method->is_static() ? "true" : "false")
           << ",params=" << method->get_num_params()
           << ",function=";
    append_pointer(stream, method->get_function());
    stream << ",return=";
    append_type(stream, return_type);
    stream << ")";
}

void append_field(std::ostringstream& stream, std::string_view label, const LoadFieldObservation& field) {
    stream << " " << label << "=";
    append_pointer(stream, field.field);
    if (field.field == nullptr) {
        return;
    }

    const auto* name = field.field->get_name();
    stream << "(declaring=";
    append_type(stream, field.declaring_type);
    stream << ",name=" << (name != nullptr ? name : "null") << ",type=";
    append_type(stream, field.field_type);
    stream << ",typeSizeMetadata=" << field.field_size
           << ",valueTypeSizeMetadata=" << field.field_value_type_size
           << ",managedIntegralWidth=" << managed_integral_width(field.storage_type)
           << ",static=" << (field.is_static ? "true" : "false")
           << ",storage=";
    append_type(stream, field.storage_type);
    stream << ",storageTypeSizeMetadata=" << field.storage_size
           << ",storageValueTypeSizeMetadata=" << field.storage_value_type_size
           << ",storageManagedIntegralWidth=" << field.storage_width << ",address=";
    append_pointer(stream, field.address);
    if (field.raw_value) {
        stream << ",rawValue=0x" << std::hex << *field.raw_value << std::dec;
    }
    stream << ")";
}

std::string format_load_accessor_observation(const GameLoadSnapshotResult& result) {
    const auto& observation = result.observation;
    std::ostringstream stream;
    stream << "stage=" << load_snapshot_failure_name(result.failure);
    stream << " threadContext=";
    append_pointer(stream, observation.thread_context);
    stream << " pauseType=";
    append_type(stream, observation.pause_type);
    append_method(stream, "pauseGetter", observation.pause_getter);
    append_field(stream, "pauseField", observation.pause_field);
    stream << " pauseManager=";
    append_pointer(stream, observation.pause_instance);
    stream << " situationType=";
    append_type(stream, observation.situation_type);
    append_method(stream, "situationGetter", observation.situation_getter);
    append_field(stream, "inhibitField", observation.inhibit_field);
    stream << " situationManager=";
    append_pointer(stream, observation.situation_instance);
    stream << " situationRuntimeType=";
    append_type(stream, observation.situation_runtime_type);
    if (result.snapshot) {
        stream << " pause=" << (result.snapshot->pause ? 1 : 0)
               << " inhibit=0x" << std::hex << result.snapshot->inhibit << std::dec;
    }
    return stream.str();
}

struct LoadAccessorLogState {
    GameLoadSnapshotResult last_result{};
    uint32_t valid_samples{};
    uint32_t transition_logs{};
    bool initialized{};
    bool suppression_logged{};
};

bool same_load_field_observation(const LoadFieldObservation& lhs, const LoadFieldObservation& rhs) {
    return lhs.field == rhs.field && lhs.declaring_type == rhs.declaring_type &&
        lhs.field_type == rhs.field_type && lhs.storage_type == rhs.storage_type &&
        lhs.address == rhs.address && lhs.field_size == rhs.field_size &&
        lhs.field_value_type_size == rhs.field_value_type_size &&
        lhs.storage_size == rhs.storage_size &&
        lhs.storage_value_type_size == rhs.storage_value_type_size &&
        lhs.storage_width == rhs.storage_width && lhs.is_static == rhs.is_static &&
        lhs.raw_value == rhs.raw_value;
}

bool same_load_accessor_observation(const GameLoadSnapshotResult& lhs, const GameLoadSnapshotResult& rhs) {
    const auto& lhs_observation = lhs.observation;
    const auto& rhs_observation = rhs.observation;
    const bool same_snapshot = lhs.snapshot.has_value() == rhs.snapshot.has_value() &&
        (!lhs.snapshot || (lhs.snapshot->pause == rhs.snapshot->pause &&
            lhs.snapshot->inhibit == rhs.snapshot->inhibit));

    return lhs.failure == rhs.failure && same_snapshot &&
        lhs_observation.pause_type == rhs_observation.pause_type &&
        lhs_observation.pause_getter == rhs_observation.pause_getter &&
        same_load_field_observation(lhs_observation.pause_field, rhs_observation.pause_field) &&
        lhs_observation.situation_type == rhs_observation.situation_type &&
        lhs_observation.situation_getter == rhs_observation.situation_getter &&
        same_load_field_observation(lhs_observation.inhibit_field, rhs_observation.inhibit_field) &&
        lhs_observation.thread_context == rhs_observation.thread_context &&
        lhs_observation.pause_instance == rhs_observation.pause_instance &&
        lhs_observation.situation_instance == rhs_observation.situation_instance &&
        lhs_observation.situation_runtime_type == rhs_observation.situation_runtime_type;
}

constexpr size_t MAX_LOAD_SCHEMA_TYPES = 16;
constexpr size_t MAX_LOAD_SCHEMA_FIELDS = 128;
constexpr size_t MAX_LOAD_SCHEMA_METHODS = 512;
constexpr size_t MAX_LOAD_SCHEMA_METHOD_LOGS = 64;
constexpr size_t MAX_LOAD_SCHEMA_STATES = 32;

template <typename T>
void hash_load_schema_value(uint64_t& hash, const T& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

void hash_load_schema_string(uint64_t& hash, std::string_view value) {
    for (const auto character : value) {
        hash ^= static_cast<uint8_t>(character);
        hash *= 1099511628211ull;
    }
    hash ^= 0xff;
    hash *= 1099511628211ull;
}

bool contains_inhibit_case_insensitive(std::string_view value) {
    constexpr std::string_view needle{ "inhibit" };
    if (value.size() < needle.size()) {
        return false;
    }

    for (size_t start = 0; start <= value.size() - needle.size(); ++start) {
        bool matches = true;
        for (size_t i = 0; i < needle.size(); ++i) {
            auto character = value[start + i];
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
            if (character != needle[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

uint64_t load_schema_signature(sdk::RETypeDefinition* runtime_type) {
    uint64_t hash = 14695981039346656037ull;
    hash_load_schema_value(hash, sdk::GameIdentity::get().tdb_ver());
    size_t field_count{};
    size_t method_count{};
    size_t type_count{};
    auto* current_type = runtime_type;

    for (; current_type != nullptr && type_count < MAX_LOAD_SCHEMA_TYPES;
         current_type = current_type->get_parent_type(), ++type_count) {
        hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(current_type));
        hash_load_schema_string(hash, current_type->get_full_name());

        const auto fields = current_type->get_fields();
        hash_load_schema_value(hash, fields.size());
        for (auto* field : fields) {
            if (field_count >= MAX_LOAD_SCHEMA_FIELDS) {
                hash_load_schema_value(hash, static_cast<uint8_t>(1));
                break;
            }
            ++field_count;
            const auto* field_name = field != nullptr ? field->get_name() : nullptr;
            auto* field_type = field != nullptr ? field->get_type() : nullptr;
            auto* declaring_type = field != nullptr ? field->get_declaring_type() : nullptr;
            hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(field));
            hash_load_schema_string(hash, field_name != nullptr ? field_name : "<null>");
            hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(declaring_type));
            hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(field_type));
            hash_load_schema_string(hash, field_type != nullptr ? field_type->get_full_name() : "<null>");
            hash_load_schema_value(hash, field != nullptr ? field->is_static() : false);
            hash_load_schema_value(hash, field != nullptr ? field->is_literal() : false);
            hash_load_schema_value(hash, field != nullptr ? field->get_offset_from_base() : 0u);
            hash_load_schema_value(hash, field != nullptr ? field->get_offset_from_fieldptr() : 0u);
            if (field_type != nullptr) {
                hash_load_schema_value(hash, field_type->get_size());
                hash_load_schema_value(hash, field_type->get_valuetype_size());
                hash_load_schema_value(hash, field_type->is_enum());
                auto* underlying_type = field_type->is_enum() ? field_type->get_underlying_type() : nullptr;
                hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(underlying_type));
                hash_load_schema_string(hash, underlying_type != nullptr ? underlying_type->get_full_name() : "<none>");
            }
        }

        const auto methods = current_type->get_methods();
        hash_load_schema_value(hash, methods.size());
        for (auto& method : methods) {
            if (method_count >= MAX_LOAD_SCHEMA_METHODS) {
                hash_load_schema_value(hash, static_cast<uint8_t>(1));
                break;
            }
            ++method_count;
            const auto* method_name = method.get_name();
            if (method_name == nullptr || !contains_inhibit_case_insensitive(method_name)) {
                continue;
            }
            auto* return_type = method.get_return_type();
            auto* declaring_type = method.get_declaring_type();
            hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(&method));
            hash_load_schema_string(hash, method_name);
            hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(declaring_type));
            hash_load_schema_value(hash, method.is_static());
            hash_load_schema_value(hash, method.get_num_params());
            hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(method.get_function()));
            hash_load_schema_value(hash, reinterpret_cast<uintptr_t>(return_type));
            hash_load_schema_string(hash, return_type != nullptr ? return_type->get_full_name() : "<null>");
        }
    }

    hash_load_schema_value(hash, type_count);
    hash_load_schema_value(hash, field_count);
    hash_load_schema_value(hash, method_count);
    hash_load_schema_value(hash, current_type != nullptr);
    return hash;
}

struct LoadAccessorSchemaState {
    sdk::RETypeDefinition* runtime_type{};
    int tdb_version{};
    uint64_t signature{};
};

void dump_load_accessor_schema_once(const LoadAccessorObservation& observation) {
    if (observation.inhibit_field.field != nullptr || observation.situation_instance == nullptr ||
        observation.situation_runtime_type == nullptr) {
        return;
    }

    static std::vector<LoadAccessorSchemaState> logged_states{};
    static bool state_limit_logged{};
    const auto tdb_version = sdk::GameIdentity::get().tdb_ver();
    const auto already_logged = std::find_if(logged_states.begin(), logged_states.end(), [&](const auto& state) {
        return state.runtime_type == observation.situation_runtime_type && state.tdb_version == tdb_version;
    });
    if (already_logged != logged_states.end()) {
        return;
    }
    if (logged_states.size() >= MAX_LOAD_SCHEMA_STATES) {
        if (!state_limit_logged) {
            state_limit_logged = true;
            spdlog::warn("[RE4XeSS][LoadAccessorSchema] further schema states suppressed after {} distinct states",
                MAX_LOAD_SCHEMA_STATES);
        }
        return;
    }
    const auto signature = load_schema_signature(observation.situation_runtime_type);
    logged_states.push_back({ observation.situation_runtime_type, tdb_version, signature });

    std::vector<sdk::RETypeDefinition*> hierarchy{};
    hierarchy.reserve(MAX_LOAD_SCHEMA_TYPES);
    for (auto* current_type = observation.situation_runtime_type;
         current_type != nullptr && hierarchy.size() < MAX_LOAD_SCHEMA_TYPES;
         current_type = current_type->get_parent_type()) {
        hierarchy.push_back(current_type);
    }
    const bool hierarchy_truncated = !hierarchy.empty() && hierarchy.size() == MAX_LOAD_SCHEMA_TYPES &&
        hierarchy.back()->get_parent_type() != nullptr;

    std::ostringstream hierarchy_stream;
    for (size_t i = 0; i < hierarchy.size(); ++i) {
        if (i != 0) hierarchy_stream << " <- ";
        hierarchy_stream << hierarchy[i]->get_full_name();
    }
    spdlog::info(
        "[RE4XeSS][LoadAccessorSchema] tdbVersion={} declaredType={} runtimeObject={} runtimeType={} runtimeEqualsDeclared={} hierarchy={} hierarchyTruncated={} metadataSignature={:016x}",
        tdb_version,
        observation.situation_type != nullptr ? observation.situation_type->get_full_name() : "<null>",
        static_cast<const void*>(observation.situation_instance),
        observation.situation_runtime_type->get_full_name(),
        observation.situation_runtime_type == observation.situation_type,
        hierarchy_stream.str(),
        hierarchy_truncated,
        static_cast<unsigned long long>(signature));

    size_t field_count{};
    bool exact_inhibit_backing_field_found{};
    bool field_dump_truncated{};
    for (auto* current_type : hierarchy) {
        for (auto* field : current_type->get_fields()) {
            if (field_count >= MAX_LOAD_SCHEMA_FIELDS) {
                field_dump_truncated = true;
                break;
            }
            ++field_count;
            if (field == nullptr) {
                spdlog::info("[RE4XeSS][LoadAccessorSchema] runtimeType={} field=<null>",
                    observation.situation_runtime_type->get_full_name());
                continue;
            }

            const auto* name = field->get_name();
            const auto* field_type = field->get_type();
            auto* underlying_type = field_type != nullptr && field_type->is_enum()
                ? field_type->get_underlying_type()
                : nullptr;
            const auto* declaring_type = field->get_declaring_type();
            exact_inhibit_backing_field_found = exact_inhibit_backing_field_found ||
                (name != nullptr && std::string_view{ name } == INHIBIT_BIT_BACKING_FIELD_NAME);
            spdlog::info(
                "[RE4XeSS][LoadAccessorSchema] field runtimeType={} declaringType={} fieldName={} fieldType={} typeSizeMetadata={} valueTypeSizeMetadata={} managedIntegralWidth={} static={} literal={} offset={} offsetFromFieldptr={} enum={} enumUnderlyingType={}",
                observation.situation_runtime_type->get_full_name(),
                declaring_type != nullptr ? declaring_type->get_full_name() : "<null>",
                name != nullptr ? name : "<null>",
                field_type != nullptr ? field_type->get_full_name() : "<null>",
                field_type != nullptr ? field_type->get_size() : 0u,
                field_type != nullptr ? field_type->get_valuetype_size() : 0u,
                managed_integral_width(field_type),
                field->is_static(), field->is_literal(),
                field->get_offset_from_base(), field->get_offset_from_fieldptr(),
                field_type != nullptr && field_type->is_enum(),
                underlying_type != nullptr ? underlying_type->get_full_name() : "<none>");
        }
        if (field_dump_truncated) break;
    }
    if (field_dump_truncated) {
        spdlog::warn("[RE4XeSS][LoadAccessorSchema] field dump truncated after {} fields",
            MAX_LOAD_SCHEMA_FIELDS);
    }
    spdlog::info(
        "[RE4XeSS][LoadAccessorSchema] exactInhibitBackingFieldFound={} fieldSearchComplete={} fieldCount={} hierarchyTruncated={}",
        exact_inhibit_backing_field_found, !field_dump_truncated && !hierarchy_truncated, field_count, hierarchy_truncated);

    if (exact_inhibit_backing_field_found || field_dump_truncated || hierarchy_truncated) {
        return;
    }

    size_t inspected_methods{};
    size_t logged_methods{};
    bool method_dump_truncated{};
    for (auto* current_type : hierarchy) {
        for (auto& method : current_type->get_methods()) {
            if (inspected_methods >= MAX_LOAD_SCHEMA_METHODS) {
                method_dump_truncated = true;
                break;
            }
            ++inspected_methods;
            const auto* name = method.get_name();
            if (name == nullptr || !contains_inhibit_case_insensitive(name)) {
                continue;
            }
            if (logged_methods >= MAX_LOAD_SCHEMA_METHOD_LOGS) {
                method_dump_truncated = true;
                break;
            }
            ++logged_methods;
            const auto* declaring_type = method.get_declaring_type();
            const auto* return_type = method.get_return_type();
            spdlog::info(
                "[RE4XeSS][LoadAccessorSchema] method declaringType={} methodName={} static={} parameterCount={} returnType={} function={}",
                declaring_type != nullptr ? declaring_type->get_full_name() : "<null>",
                name, method.is_static(), method.get_num_params(),
                return_type != nullptr ? return_type->get_full_name() : "<null>",
                method.get_function());
        }
        if (method_dump_truncated) break;
    }
    if (method_dump_truncated) {
        spdlog::warn("[RE4XeSS][LoadAccessorSchema] inhibit-related method dump truncated after {} inspected and {} logged",
            inspected_methods, logged_methods);
    }
}

void log_load_accessor_observation(const GameLoadSnapshotResult& result) {
    const auto config = REFrameworkConfig::get();
    if (config == nullptr || !config->is_debug_log_enabled()) {
        return;
    }

    dump_load_accessor_schema_once(result.observation);

    static LoadAccessorLogState state{};
    const bool changed = !state.initialized || !same_load_accessor_observation(state.last_result, result);
    const bool valid_sample = result.snapshot.has_value() && state.valid_samples < 32;
    if (!changed && !valid_sample) {
        return;
    }

    const bool transition_limit_reached = changed && state.initialized && state.transition_logs >= 128;
    if (transition_limit_reached && !valid_sample) {
        if (!state.suppression_logged) {
            state.suppression_logged = true;
            spdlog::warn("[RE4XeSS][LoadAccessor] further state-transition diagnostics suppressed after 128 changes");
        }
        state.last_result = result;
        return;
    }

    spdlog::info("[RE4XeSS][LoadAccessor] {}", format_load_accessor_observation(result));
    if (changed && state.initialized && state.transition_logs < 128) {
        ++state.transition_logs;
    }
    if (valid_sample) {
        ++state.valid_samples;
    }
    state.last_result = result;
    state.initialized = true;
}

bool valid_zero_parameter_static_getter(sdk::REMethodDefinition* method) {
    return method != nullptr && method->get_function() != nullptr &&
        method->is_static() && method->get_num_params() == 0;
}

GameLoadSnapshotResult read_game_load_snapshot() {
    static LoadStateAccessors accessors{};
    accessors.resolve();

    GameLoadSnapshotResult result{};
    auto& observation = result.observation;
    observation.pause_type = accessors.pause_type;
    observation.pause_getter = accessors.pause_instance_getter;
    observation.situation_type = accessors.situation_type;
    observation.situation_getter = accessors.situation_instance_getter;
    observe_field_metadata(observation.pause_field, accessors.pause_field);
    observe_field_metadata(observation.inhibit_field, accessors.inhibit_field);
    observation.thread_context = sdk::get_thread_context();

    if (observation.pause_type == nullptr) {
        result.failure = LoadSnapshotFailure::PauseTypeUnavailable;
        return result;
    }
    if (observation.pause_getter == nullptr) {
        result.failure = LoadSnapshotFailure::PauseGetterUnavailable;
        return result;
    }
    if (observation.pause_field.field == nullptr) {
        result.failure = LoadSnapshotFailure::PauseFieldUnavailable;
        return result;
    }
    if (observation.situation_type == nullptr) {
        result.failure = LoadSnapshotFailure::SituationTypeUnavailable;
        return result;
    }
    if (observation.situation_getter == nullptr) {
        result.failure = LoadSnapshotFailure::SituationGetterUnavailable;
        return result;
    }
    if (!valid_zero_parameter_static_getter(observation.pause_getter)) {
        result.failure = LoadSnapshotFailure::PauseGetterInvalidSignature;
        return result;
    }
    if (!valid_zero_parameter_static_getter(observation.situation_getter)) {
        result.failure = LoadSnapshotFailure::SituationGetterInvalidSignature;
        return result;
    }

    if (observation.thread_context == nullptr) {
        result.failure = LoadSnapshotFailure::ThreadContextUnavailable;
        return result;
    }

    observation.pause_instance = observation.pause_getter->call_safe<REManagedObject*>(observation.thread_context);
    observation.situation_instance = observation.situation_getter->call_safe<REManagedObject*>(observation.thread_context);
    if (observation.situation_instance != nullptr) {
        observation.situation_runtime_type = observation.situation_instance->get_type_definition();
    }
    if (observation.pause_instance == nullptr) {
        result.failure = LoadSnapshotFailure::PauseInstanceUnavailable;
        return result;
    }
    if (observation.situation_instance == nullptr) {
        result.failure = LoadSnapshotFailure::SituationInstanceUnavailable;
        return result;
    }
    if (observation.inhibit_field.field == nullptr) {
        result.failure = LoadSnapshotFailure::InhibitFieldUnavailable;
        return result;
    }

    const auto pause = read_bool_field(observation.pause_field, observation.pause_instance);
    const auto inhibit = read_integral_field(observation.inhibit_field, observation.situation_instance);
    if (pause.failure != LoadSnapshotFailure::None) {
        result.failure = pause.failure;
        return result;
    }
    if (inhibit.failure != LoadSnapshotFailure::None) {
        result.failure = inhibit.failure;
        return result;
    }

    result.snapshot = GameLoadSnapshot{ *pause.value, *inhibit.value };
    return result;
}

std::array<sdk::renderer::SceneInfo*, 6> scene_infos(sdk::renderer::layer::Scene* layer) {
    return {
        layer->get_scene_info(),
        layer->get_depth_distortion_scene_info(),
        layer->get_filter_scene_info(),
        layer->get_jitter_disable_scene_info(),
        layer->get_jitter_disable_post_scene_info(),
        layer->get_z_prepass_scene_info(),
    };
}

constexpr std::array<std::string_view, 6> SCENE_INFO_NAMES{
    "main", "depthDistortion", "filter", "jitterDisable", "jitterDisablePost", "zPrepass",
};

class CreateRenderTargetViewProbe final {
public:
    using Function = void(STDMETHODCALLTYPE*)(
        ID3D12Device4*,
        ID3D12Resource*,
        const D3D12_RENDER_TARGET_VIEW_DESC*,
        D3D12_CPU_DESCRIPTOR_HANDLE);

    static CreateRenderTargetViewProbe& instance() {
        static CreateRenderTargetViewProbe probe{};
        return probe;
    }

    bool ensure(ID3D12Device4* device, uint64_t handoff_frame, uint32_t output_width, uint32_t output_height) {
        if (device == nullptr) {
            reset();
            return false;
        }

        if (s_active_device.load(std::memory_order_acquire) == device) {
            if (limits_reached()) {
                std::lock_guard lock{ m_mutex };
                if (m_device.Get() == device) {
                    detach_locked();
                }
                log_limit_once();
                return false;
            }
            return true;
        }

        std::lock_guard lock{ m_mutex };
        if (m_device.Get() == device && m_hook != nullptr) {
            return !limits_reached();
        }

        detach_locked();
        if (limits_reached()) {
            log_limit_once();
            return false;
        }

        [[maybe_unused]] Microsoft::WRL::ComPtr<ID3D12Device4> device_keepalive{ device };
        auto hook = std::make_unique<VtableHook>();
        if (!hook->create(Address{ device })) {
            spdlog::warn("[RE4XeSS][RTVProbe] could not copy the D3D12 device vtable");
            return false;
        }

        constexpr uint32_t create_render_target_view_slot = 20;
        const auto original = hook->get_method<Function>(create_render_target_view_slot);
        if (original == nullptr || !register_original(device, original)) {
            spdlog::warn("[RE4XeSS][RTVProbe] could not register the original CreateRenderTargetView method");
            return false;
        }

        m_device = device;
        m_hook = std::move(hook);
        s_active_device.store(device, std::memory_order_release);
        if (!m_hook->hook_method(
                create_render_target_view_slot,
                Address{ reinterpret_cast<void*>(&CreateRenderTargetViewProbe::create_render_target_view) })) {
            s_active_device.store(nullptr, std::memory_order_release);
            unregister_original(device, original);
            detach_locked();
            spdlog::warn("[RE4XeSS][RTVProbe] could not hook the D3D12 device method");
            return false;
        }

        spdlog::info("[RE4XeSS][RTVProbe] armed for output-handoff attempt frame={} expectedOutput={}x{} device=0x{:x} vtableSlot={} captureLimit={} callLimit={}",
            static_cast<unsigned long long>(handoff_frame),
            output_width,
            output_height,
            reinterpret_cast<uintptr_t>(device),
            create_render_target_view_slot,
            MAX_CAPTURES,
            MAX_CALLS);
        return true;
    }

    void reset() {
        if (s_active_device.load(std::memory_order_acquire) == nullptr) {
            return;
        }

        std::lock_guard lock{ m_mutex };
        detach_locked();
    }

private:
    struct OriginalEntry {
        ID3D12Device4* device{};
        Function function{};
    };

    static constexpr uint32_t MAX_CAPTURES = 8;
    static constexpr uint32_t MAX_CALLS = 512;
    static constexpr size_t MAX_REGISTERED_DEVICES = 4;
    static constexpr USHORT MAX_STACK_FRAMES = 16;

    static void STDMETHODCALLTYPE create_render_target_view(
        ID3D12Device4* device,
        ID3D12Resource* resource,
        const D3D12_RENDER_TARGET_VIEW_DESC* description,
        D3D12_CPU_DESCRIPTOR_HANDLE destination) {
        auto& probe = instance();
        const auto original = probe.find_original(device);
        if (original == nullptr) {
            spdlog::error("[RE4XeSS][RTVProbe] original CreateRenderTargetView was unavailable; call skipped");
            return;
        }

        original(device, resource, description, destination);

        if (s_active_device.load(std::memory_order_acquire) == device) {
            probe.capture(device, resource, description);
        }
    }

    static bool register_original(ID3D12Device4* device, Function function) {
        std::lock_guard lock{ s_registry_mutex };
        for (auto& entry : s_originals) {
            if (entry.device == device) {
                entry.function = function;
                return true;
            }
        }

        for (auto& entry : s_originals) {
            if (entry.device == nullptr) {
                entry = { device, function };
                return true;
            }
        }
        return false;
    }

    static Function find_original(ID3D12Device4* device) {
        std::lock_guard lock{ s_registry_mutex };
        for (const auto& entry : s_originals) {
            if (entry.device == device) {
                return entry.function;
            }
        }
        return nullptr;
    }

    static void unregister_original(ID3D12Device4* device, Function function) {
        std::lock_guard lock{ s_registry_mutex };
        for (auto& entry : s_originals) {
            if (entry.device == device && entry.function == function) {
                entry = {};
                return;
            }
        }
    }

    static bool limits_reached() noexcept {
        return s_capture_count.load(std::memory_order_acquire) >= MAX_CAPTURES ||
            s_call_count.load(std::memory_order_acquire) >= MAX_CALLS;
    }

    static void log_limit_once() {
        if (!s_limit_logged.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("[RE4XeSS][RTVProbe] disarmed after captures={} calls={}",
                s_capture_count.load(std::memory_order_acquire),
                s_call_count.load(std::memory_order_acquire));
        }
    }

    static std::pair<uintptr_t, uintptr_t> main_module_range() noexcept {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (base == 0) {
            return {};
        }

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return {};
        }

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0) {
            return {};
        }
        return { base, base + nt->OptionalHeader.SizeOfImage };
    }

    void capture(
        ID3D12Device4* device,
        ID3D12Resource* resource,
        const D3D12_RENDER_TARGET_VIEW_DESC* description) noexcept {
        const auto call_index = s_call_count.fetch_add(1, std::memory_order_acq_rel);
        if (call_index >= MAX_CALLS || s_capture_count.load(std::memory_order_acquire) >= MAX_CAPTURES) {
            return;
        }

        try {
            std::array<void*, MAX_STACK_FRAMES> frames{};
            const auto frame_count = RtlCaptureStackBackTrace(
                0,
                MAX_STACK_FRAMES,
                frames.data(),
                nullptr);
            const auto [module_begin, module_end] = main_module_range();
            bool has_game_frame = false;
            for (USHORT index = 0; index < frame_count; ++index) {
                const auto address = reinterpret_cast<uintptr_t>(frames[index]);
                has_game_frame = has_game_frame || (module_begin != 0 && address >= module_begin && address < module_end);
            }
            if (!has_game_frame) {
                return;
            }

            const auto capture_index = s_capture_count.fetch_add(1, std::memory_order_acq_rel);
            if (capture_index >= MAX_CAPTURES) {
                return;
            }

            std::ostringstream stack;
            stack << std::hex;
            for (USHORT index = 0; index < frame_count; ++index) {
                if (index != 0) {
                    stack << ',';
                }
                const auto address = reinterpret_cast<uintptr_t>(frames[index]);
                if (module_begin != 0 && address >= module_begin && address < module_end) {
                    stack << "re4+0x" << (address - module_begin);
                } else {
                    stack << "0x" << address;
                }
            }

            const auto format = description == nullptr ? 0 : static_cast<uint32_t>(description->Format);
            const auto dimension = description == nullptr ? 0 : static_cast<uint32_t>(description->ViewDimension);
            spdlog::info("[RE4XeSS][RTVProbe] capture={} attempt={} device=0x{:x} resource=0x{:x} format={} dimension={} stack=[{}]",
                capture_index + 1,
                call_index + 1,
                reinterpret_cast<uintptr_t>(device),
                reinterpret_cast<uintptr_t>(resource),
                format,
                dimension,
                stack.str());
        } catch (...) {
            // Diagnostics must never change the D3D12 call's behavior.
        }
    }

    void detach_locked() {
        s_active_device.store(nullptr, std::memory_order_release);
        m_hook.reset();
        m_device.Reset();
    }

    std::mutex m_mutex{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    std::unique_ptr<VtableHook> m_hook{};

    static inline std::mutex s_registry_mutex{};
    static inline std::array<OriginalEntry, MAX_REGISTERED_DEVICES> s_originals{};
    static inline std::atomic<ID3D12Device4*> s_active_device{};
    static inline std::atomic<uint32_t> s_call_count{};
    static inline std::atomic<uint32_t> s_capture_count{};
    static inline std::atomic<bool> s_limit_logged{};
};

class TargetStateFactoryProbe final {
public:
    static TargetStateFactoryProbe& instance() {
        static TargetStateFactoryProbe probe{};
        return probe;
    }

    struct DiscoveryResult {
        bool attempted{};
        bool complete{};
        size_t xref_count{};
        bool creator_probe_armed{};
    };

    struct ImageIdentity {
        uintptr_t base{};
        uint32_t size{};
        uint32_t checksum{};
        bool valid{};
        bool expected_re4_build{};
    };

    ImageIdentity image_identity() const noexcept {
        const auto image = inspect_main_image();
        return {
            image.base,
            image.size,
            image.checksum,
            image.valid,
            image.valid && image.size == EXPECTED_IMAGE_SIZE && image.checksum == EXPECTED_IMAGE_CHECKSUM,
        };
    }

    struct CreatorObjectSnapshot {
        bool readable{};
        uintptr_t object{};
        uintptr_t vtable{};
        int32_t ref_count{};
        uint32_t render_frame{};
        uintptr_t rtvs{};
        uintptr_t dsv{};
        uint32_t num_rtv{};
        float rect_left{};
        float rect_top{};
        float rect_right{};
        float rect_bottom{};
        uint32_t flag{};
        uintptr_t rtv0{};
        bool rtv_valid{};
        uint32_t rtv_format{};
        uint32_t rtv_dimension{};
        uintptr_t texture{};
        bool texture_header_valid{};
        uintptr_t texture_vtable{};
        int32_t texture_ref_count{};
        uint32_t texture_render_frame{};
        bool target_state_like{};
    };

    DiscoveryResult discover_target_state_vtable_early() noexcept {
        if (!sdk::GameIdentity::get().is_re4()) {
            return {};
        }

        const auto image = inspect_main_image();
        if (!image.valid || image.size != EXPECTED_IMAGE_SIZE || image.checksum != EXPECTED_IMAGE_CHECKSUM) {
            spdlog::warn("[RE4XeSS][TargetStateVtableProbe] discovery rejected: RE4 image identity mismatch size=0x{:x} checksum=0x{:x}",
                image.size, image.checksum);
            return {};
        }

        m_image_base = image.base;
        m_image_end = image.base + image.size;
        m_image_identity_valid.store(true, std::memory_order_release);
        m_expected_vtable.store(image.base + TARGET_STATE_VTABLE_RVA, std::memory_order_release);
        auto result = discover_vtable_xrefs(image);
        result.creator_probe_armed = arm_creator_probe_early(image);
        return result;
    }

    bool arm(
        uint64_t frame_id,
        sdk::renderer::layer::Overlay* overlay,
        sdk::renderer::TargetState* overlay_state,
        ID3D12Resource* semantic_color) {
        if (!sdk::GameIdentity::get().is_re4() || overlay == nullptr || overlay_state == nullptr || semantic_color == nullptr) {
            return false;
        }

        const auto config = REFrameworkConfig::get();
        if (config == nullptr || !config->is_debug_log_enabled()) {
            return false;
        }

        (void)refresh_live_overlay_anchor(overlay, overlay_state, frame_id);
        if (!m_live_anchor_trusted.load(std::memory_order_acquire)) {
            spdlog::warn("[RE4XeSS][TargetStateProbe] not armed: live TargetState vtable anchor is untrusted");
            return false;
        }
        if (m_attempted.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }

        const auto image = inspect_main_image();
        if (!image.valid || image.size != EXPECTED_IMAGE_SIZE || image.checksum != EXPECTED_IMAGE_CHECKSUM) {
            spdlog::warn("[RE4XeSS][TargetStateProbe] not armed: RE4 1.5.9.0 image identity mismatch size=0x{:x} checksum=0x{:x}",
                image.size, image.checksum);
            return false;
        }

        for (size_t index = 0; index < CALLSITE_RVAS.size(); ++index) {
            if (!validate_callsite(image, index)) {
                spdlog::warn("[RE4XeSS][TargetStateProbe] not armed: callsite validation failed index={} callsiteRva=0x{:x}",
                    index, CALLSITE_RVAS[index]);
                return false;
            }
        }

        m_image_base = image.base;
        m_image_end = image.base + image.size;
        m_frame_id = frame_id;
        m_arm_thread_id = GetCurrentThreadId();
        m_semantic_color = reinterpret_cast<uintptr_t>(semantic_color);
        m_calls_captured.store(0, std::memory_order_release);
        m_returns_captured.store(0, std::memory_order_release);
        m_pending_call_count.store(0, std::memory_order_release);
        m_present_grace_count.store(0, std::memory_order_release);
        m_next_call_id.store(0, std::memory_order_release);
        m_capture_started.store(false, std::memory_order_release);
        m_capture_stopped.store(false, std::memory_order_release);
        for (size_t index = 0; index < SITE_COUNT; ++index) {
            m_site_call_counts[index].store(0, std::memory_order_release);
            m_site_return_counts[index].store(0, std::memory_order_release);
            m_return_hook_entries[index].store(0, std::memory_order_release);
            m_unmatched_returns[index].store(0, std::memory_order_release);
        }

        for (size_t index = 0; index < HOOK_COUNT; ++index) {
            if (!selected_hook(index)) {
                continue;
            }
            const auto address = reinterpret_cast<void*>(image.base + hook_rva(index));
            m_hooks[index] = safetyhook::create_mid(address, hook_callback(index), safetyhook::MidHook::StartDisabled);
            if (!m_hooks[index]) {
                disarm("could not create all callsite hooks");
                return false;
            }
        }

        // The pre-hooks must stop before each original CALL so its address and return address stay unchanged.
        for (size_t index = 0; index < SITE_COUNT; ++index) {
            if (!selected_site(index)) {
                continue;
            }
            const auto expected_span = CALLSITE_RVAS[index] - PRE_HOOK_RVAS[index];
            const auto actual_span = m_hooks[index].original_bytes().size();
            if (actual_span == 0 || actual_span > expected_span) {
                spdlog::warn("[RE4XeSS][TargetStateProbe] not armed: pre-hook span would overlap callsite index={} expectedMax={} actual={}",
                    index, expected_span, actual_span);
                disarm("pre-hook span overlaps callsite");
                return false;
            }
            spdlog::info("[RE4XeSS][TargetStateProbe] pre-hook span validated index={} startRva=0x{:x} callsiteRva=0x{:x} expectedMax={} actual={} callsitePreserved=true",
                index, PRE_HOOK_RVAS[index], CALLSITE_RVAS[index], expected_span, actual_span);
        }

        for (size_t index = 0; index < HOOK_COUNT; ++index) {
            if (m_hooks[index] && !m_hooks[index].enable()) {
                disarm("could not enable all callsite hooks");
                return false;
            }
        }

        m_active.store(true, std::memory_order_release);

        spdlog::info("[RE4XeSS][TargetStateProbe] armed frame={} thread={} imageSize=0x{:x} checksum=0x{:x} overlayState=0x{:x} desc=0x{:x} rtvArray=0x{:x} rtv0=0x{:x} color=0x{:x} validatedSites={} activePairs={} isolatedSite={} siteName={} captureLimit={} perSiteLimit={} pendingPresentGrace={} captureStarts=after-prepare",
            static_cast<unsigned long long>(m_frame_id),
            m_arm_thread_id,
            image.size,
            image.checksum,
            m_overlay_state,
            m_overlay_desc,
            m_overlay_rtv_array,
            m_overlay_rtv,
            m_semantic_color,
            CALLSITE_RVAS.size(),
            selected_site(ISOLATED_SITE_INDEX) ? 1 : SITE_COUNT,
            ISOLATED_SITE_INDEX < SITE_COUNT ? static_cast<int>(ISOLATED_SITE_INDEX) : -1,
            ISOLATED_SITE_INDEX < SITE_COUNT ? SITE_NAMES[ISOLATED_SITE_INDEX] : "all",
            MAX_CAPTURED_CALLS,
            MAX_CAPTURED_CALLS_PER_SITE,
            MAX_PENDING_PRESENT_GRACE);
        log_overlay_state_snapshot();
        return true;
    }

    void on_prepare_return(bool prepared) noexcept {
        if (!m_active.load(std::memory_order_acquire)) {
            return;
        }

        bool expected = false;
        if (!m_capture_started.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
        spdlog::info("[RE4XeSS][TargetStateProbe] capture window started frame={} prepare={} thread={} boundary=next-post-present",
            static_cast<unsigned long long>(m_frame_id),
            prepared,
            GetCurrentThreadId());
    }

    void disarm(std::string_view reason) noexcept {
        m_active.store(false, std::memory_order_release);
        if (m_disarm_started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        bool disable_failed{};
        for (auto& hook : m_hooks) {
            if (hook && !hook.disable()) {
                disable_failed = true;
            }
        }

        spdlog::info("[RE4XeSS][TargetStateProbe] disarmed reason={} captureStarted={} capturedCalls={} returnedCalls={} pendingCalls={} pendingPresentGrace={} captureBudgetReached={} disableFailed={}",
            reason,
            m_capture_started.load(std::memory_order_acquire),
            m_calls_captured.load(std::memory_order_acquire),
            m_returns_captured.load(std::memory_order_acquire),
            m_pending_call_count.load(std::memory_order_acquire),
            m_present_grace_count.load(std::memory_order_acquire),
            m_capture_stopped.load(std::memory_order_acquire),
            disable_failed);
        for (size_t index = 0; index < SITE_COUNT; ++index) {
            spdlog::info("[RE4XeSS][TargetStateProbe] site-summary site={} callsiteRva=0x{:x} pre={} post={} postHookEntries={} unmatchedPost={}",
                SITE_NAMES[index],
                CALLSITE_RVAS[index],
                m_site_call_counts[index].load(std::memory_order_acquire),
                m_site_return_counts[index].load(std::memory_order_acquire),
                m_return_hook_entries[index].load(std::memory_order_acquire),
                m_unmatched_returns[index].load(std::memory_order_acquire));
        }
    }

    void force_disarm_all(std::string_view reason) noexcept {
        disarm(reason);
        disarm_creator_probe(reason);
    }

    CreatorObjectSnapshot snapshot_creator_object(uintptr_t object) const noexcept;

    void refresh_live_overlay_slot(sdk::renderer::layer::Overlay* overlay) noexcept {
        if (!m_vtable_discovery_done.load(std::memory_order_acquire) || overlay == nullptr) {
            return;
        }

        auto* state = overlay->get_main_target_state().get();
        if (refresh_live_overlay_anchor(overlay, state, 0)) {
            correlate_creator_observations(reinterpret_cast<uintptr_t>(state));
        }
    }

    void on_post_present() noexcept {
        if (m_creator_active.load(std::memory_order_acquire)) {
            const auto frame_count = m_creator_frames.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (m_creator_call_count.load(std::memory_order_acquire) >= MAX_CREATOR_OBSERVATIONS ||
                frame_count >= MAX_CREATOR_CAPTURE_FRAMES) {
                disarm_creator_probe(m_creator_call_count.load(std::memory_order_acquire) >= MAX_CREATOR_OBSERVATIONS
                        ? "observation budget reached"
                        : "frame budget reached");
            }
        }
        if (!m_active.load(std::memory_order_acquire)) {
            return;
        }

        const auto pending_calls = m_pending_call_count.load(std::memory_order_acquire);
        if (pending_calls == 0) {
            disarm(m_capture_stopped.load(std::memory_order_acquire)
                    ? "capture budget reached; hooks removed at post-present boundary"
                    : "post-present frame boundary with no pending calls");
            return;
        }

        const auto grace_count = m_present_grace_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (grace_count >= MAX_PENDING_PRESENT_GRACE) {
            disarm(m_capture_stopped.load(std::memory_order_acquire)
                    ? "capture budget reached; pending-call post-present grace exhausted"
                    : "pending-call post-present grace exhausted");
            return;
        }

        spdlog::info("[RE4XeSS][TargetStateProbe] retaining hooks across post-present boundary pendingCalls={} grace={}/{} captureBudgetReached={}",
            pending_calls, grace_count, MAX_PENDING_PRESENT_GRACE,
            m_capture_stopped.load(std::memory_order_acquire));
    }

    void configure_overlay_writer_probe(bool enabled, bool diagnostics_enabled) noexcept {
        m_overlay_writer_enabled.store(enabled, std::memory_order_release);
        m_overlay_writer_diagnostics_enabled.store(diagnostics_enabled, std::memory_order_release);
        if (!enabled) {
            m_overlay_writer_transaction_enabled.store(false, std::memory_order_release);
            m_overlay_writer_diagnostic_capture.store(false, std::memory_order_release);
        }
        if (diagnostics_enabled) {
            spdlog::info("[RE4XeSS][HandoffProvenance] writerProbe={} reason={}",
                enabled ? "awaiting-live-overlay-anchor" : "disabled",
                enabled ? "waiting-for-first-validated-overlay-callback" : "provenance-activation-rejected");
        }
    }

    void note_overlay_writer_transition(
        uint64_t generation,
        int32_t mode,
        uint64_t device_reset_generation) noexcept {
        if (!m_overlay_writer_enabled.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard lock{ m_overlay_writer_transition_mutex };
        m_overlay_writer_mode.store(mode, std::memory_order_release);
        m_overlay_writer_device_reset_generation.store(device_reset_generation, std::memory_order_release);
        // Publish generation last: callbacks that observe it with acquire also see this tuple.
        m_overlay_writer_generation.store(generation, std::memory_order_release);
        m_overlay_writer_witness_history.reset();
        m_overlay_writer_events.store(0, std::memory_order_release);
        m_overlay_writer_confirmed_writes.store(0, std::memory_order_release);
        m_overlay_writer_unconfirmed_writes.store(0, std::memory_order_release);
        m_overlay_writer_confirmed_transactions.store(0, std::memory_order_release);
        m_overlay_writer_transaction_overflow.store(0, std::memory_order_release);
        m_overlay_writer_frames.store(0, std::memory_order_release);
        m_overlay_writer_transaction_enabled.store(true, std::memory_order_release);
        m_overlay_writer_diagnostic_capture.store(true, std::memory_order_release);
    }

    re4_xess::EngineWriterWitnessChain confirmed_overlay_writer_witness_chain() const noexcept {
        const auto callbacks_before = m_overlay_writer_callback_in_flight.load(std::memory_order_acquire);
        const auto sequence_before = m_overlay_writer_store_sequence.load(std::memory_order_acquire);
        auto chain = m_overlay_writer_witness_history.snapshot();
        const auto sequence_after = m_overlay_writer_store_sequence.load(std::memory_order_acquire);
        const auto callbacks_after = m_overlay_writer_callback_in_flight.load(std::memory_order_acquire);
        chain.current_store_sequence = sequence_after;
        chain.live_store_sequence = &m_overlay_writer_store_sequence;
        chain.stable = callbacks_before == 0 && callbacks_after == 0 && sequence_before == sequence_after;
        for (size_t index = 1; index < chain.count; ++index) {
            auto witness = chain.entries[index];
            auto position = index;
            while (position > 0 &&
                witness.clear_pre_sequence < chain.entries[position - 1].clear_pre_sequence) {
                chain.entries[position] = chain.entries[position - 1];
                --position;
            }
            chain.entries[position] = witness;
        }
        return chain;
    }

    void consume_overlay_writer_witnesses_through(uint64_t terminal_store_sequence) noexcept {
        m_overlay_writer_witness_history.consume_through(terminal_store_sequence);
    }

    void observe_overlay_writer(sdk::renderer::layer::Overlay* overlay) noexcept {
        if (!m_overlay_writer_enabled.load(std::memory_order_acquire) || overlay == nullptr) {
            return;
        }
        const auto overlay_address = reinterpret_cast<uintptr_t>(overlay);
        if (m_overlay_writer_diagnostic_capture.load(std::memory_order_acquire) &&
            m_overlay_writer_frames.fetch_add(1, std::memory_order_acq_rel) == 63) {
            m_overlay_writer_diagnostic_capture.store(false, std::memory_order_release);
            if (m_overlay_writer_diagnostics_enabled.load(std::memory_order_acquire)) {
                spdlog::info("[RE4XeSS][OverlayWriter] diagnostic-capture-ended generation={} frames=64 attemptedStores={} confirmedWrites={} unconfirmedWrites={} confirmedTransactions={} transactionOverflow={} (production transaction observation remains generation-scoped)",
                    m_overlay_writer_generation.load(std::memory_order_acquire),
                    m_overlay_writer_events.load(std::memory_order_acquire),
                    m_overlay_writer_confirmed_writes.load(std::memory_order_acquire),
                    m_overlay_writer_unconfirmed_writes.load(std::memory_order_acquire),
                    m_overlay_writer_confirmed_transactions.load(std::memory_order_acquire),
                    m_overlay_writer_transaction_overflow.load(std::memory_order_acquire));
            }
        }

        if (m_overlay_writer_active.load(std::memory_order_acquire)) {
            m_overlay_writer_overlay.store(overlay_address, std::memory_order_release);
            return;
        }
        if (m_overlay_writer_permanently_unavailable.load(std::memory_order_acquire)) {
            return;
        }

        std::unique_lock install_lock{ m_overlay_writer_install_mutex, std::try_to_lock };
        if (!install_lock.owns_lock() ||
            !m_overlay_writer_enabled.load(std::memory_order_acquire)) {
            return;
        }
        if (m_overlay_writer_active.load(std::memory_order_acquire)) {
            m_overlay_writer_overlay.store(overlay_address, std::memory_order_release);
            return;
        }
        if (m_overlay_writer_permanently_unavailable.load(std::memory_order_acquire)) {
            return;
        }

        constexpr uint32_t MAX_OVERLAY_WRITER_INSTALL_ATTEMPTS = 8;
        ++m_overlay_writer_install_attempts;
        const auto install_attempt = m_overlay_writer_install_attempts;
        const auto diagnostics_enabled = m_overlay_writer_diagnostics_enabled.load(std::memory_order_acquire);

        const auto image = inspect_main_image();
        const auto mark_permanently_unavailable = [&](const char* reason) {
            m_overlay_writer_last_install_failure = reason;
            m_overlay_writer_permanently_unavailable.store(true, std::memory_order_release);
            spdlog::warn("[RE4XeSS][OverlayWriter] unavailable reason={} attempt={} imageSize=0x{:x} checksum=0x{:x} overlay=0x{:x}; engine replacement remains fail-closed",
                reason, install_attempt, image.size, image.checksum, overlay_address);
        };
        const auto report_retryable_failure = [&](const char* reason) {
            m_overlay_writer_last_install_failure = reason;
            if (install_attempt >= MAX_OVERLAY_WRITER_INSTALL_ATTEMPTS) {
                m_overlay_writer_permanently_unavailable.store(true, std::memory_order_release);
                spdlog::warn("[RE4XeSS][OverlayWriter] unavailable reason=retry-budget-exhausted lastFailure={} attempts={} overlay=0x{:x}; engine replacement remains fail-closed",
                    reason, install_attempt, overlay_address);
            } else if (diagnostics_enabled) {
                spdlog::info("[RE4XeSS][OverlayWriter] retryable-not-armed reason={} attempt={}/{} overlay=0x{:x}",
                    reason, install_attempt, MAX_OVERLAY_WRITER_INSTALL_ATTEMPTS, overlay_address);
            }
        };
        const auto slot_address = reinterpret_cast<uintptr_t>(&overlay->get_main_target_state());
        uintptr_t overlay_vtable{};
        uintptr_t writer_method{};
        if (!image.valid) {
            report_retryable_failure("main-image-metadata-unavailable");
            return;
        }
        if (image.size != EXPECTED_IMAGE_SIZE || image.checksum != EXPECTED_IMAGE_CHECKSUM) {
            mark_permanently_unavailable("unsupported-re4-image");
            return;
        }
        if (!is_executable_range(image, 0x44AF030, 0x449) ||
            !is_executable_range(image, 0x44704D0, 0x18) ||
            !matches_bytes(image.base + 0x44704D0, { 0x48, 0x8B, 0x0D, 0xB9, 0x08, 0x3E, 0x09 }) ||
            !matches_bytes(image.base + 0x44704E3, { 0xE9, 0x98, 0x1C, 0x36, 0x00 }) ||
            !matches_bytes(image.base + 0x44AF054, { 0x48, 0x8B, 0xF1 }) ||
            !matches_bytes(image.base + 0x44AF13C, { 0xE8, 0x8F, 0x13, 0xFC, 0xFF }) ||
            !matches_bytes(image.base + 0x44AF161, { 0x4C, 0x89, 0xB6, 0x90, 0x00, 0x00, 0x00 }) ||
            !matches_bytes(image.base + 0x44AF168, { 0x48, 0x85, 0xC9, 0x74, 0x05 }) ||
            !matches_bytes(image.base + 0x44AF179, { 0x48, 0x89, 0x9E, 0x90, 0x00, 0x00, 0x00 }) ||
            !matches_bytes(image.base + 0x44AF180, { 0x48, 0x85, 0xC9, 0x74, 0x05 }) ||
            !matches_bytes(image.base + 0x44AF43B, { 0xE8, 0x90, 0x10, 0xFC, 0xFF }) ||
            !matches_bytes(image.base + 0x44AF460, { 0x4C, 0x89, 0xB6, 0x90, 0x00, 0x00, 0x00 }) ||
            !matches_bytes(image.base + 0x44AF467, { 0x48, 0x85, 0xC9, 0x74, 0x05 }) ||
            !matches_bytes(image.base + 0x44AF478, { 0x48, 0x89, 0xBE, 0x90, 0x00, 0x00, 0x00 }) ||
            !matches_bytes(image.base + 0x44AF47F, { 0x48, 0x85, 0xC9, 0x74, 0x05 })) {
            mark_permanently_unavailable("writer-instruction-validation-failed");
            return;
        }
        if (slot_address != overlay_address + RE4_TARGET_STATE_SLOT_OFFSET ||
            !read_private_memory(overlay_address, &overlay_vtable, sizeof(overlay_vtable)) ||
            overlay_vtable > UINTPTR_MAX - 0x40 ||
            !read_memory(overlay_vtable + 0x40, &writer_method, sizeof(writer_method)) ||
            writer_method != image.base + 0x44AF030) {
            report_retryable_failure("overlay-vtable-or-slot-not-ready");
            return;
        }

        m_overlay_writer_overlay.store(overlay_address, std::memory_order_release);
        constexpr std::array<safetyhook::MidHookFn, OVERLAY_WRITER_HOOK_COUNT> callbacks{
            &overlay_writer_callback<0>, &overlay_writer_callback<1>,
            &overlay_writer_callback<2>, &overlay_writer_callback<3>,
            &overlay_writer_callback<4>, &overlay_writer_callback<5>,
            &overlay_writer_callback<6>, &overlay_writer_callback<7>,
        };
        std::array<safetyhook::MidHook, OVERLAY_WRITER_HOOK_COUNT> pending_hooks{};
        for (size_t index = 0; index < OVERLAY_WRITER_HOOK_RVAS.size(); ++index) {
            pending_hooks[index] = safetyhook::create_mid(
                reinterpret_cast<void*>(image.base + OVERLAY_WRITER_HOOK_RVAS[index]), callbacks[index],
                safetyhook::MidHook::StartDisabled);
            if (!pending_hooks[index]) {
                if (!rollback_overlay_writer_hooks(pending_hooks, image.base)) {
                    m_overlay_writer_hooks = std::move(pending_hooks);
                    mark_permanently_unavailable("hook-creation-rollback-unconfirmed");
                    return;
                }
                report_retryable_failure("hook-creation-failed-rollback-confirmed");
                return;
            }
            if (pending_hooks[index].original_bytes().size() != OVERLAY_WRITER_HOOK_SPANS[index]) {
                if (!rollback_overlay_writer_hooks(pending_hooks, image.base)) {
                    m_overlay_writer_hooks = std::move(pending_hooks);
                    mark_permanently_unavailable("hook-span-mismatch-rollback-unconfirmed");
                    return;
                }
                mark_permanently_unavailable("hook-span-mismatch");
                return;
            }
        }

        for (auto& hook : pending_hooks) {
            if (!hook.enable()) {
                if (!rollback_overlay_writer_hooks(pending_hooks, image.base)) {
                    m_overlay_writer_hooks = std::move(pending_hooks);
                    mark_permanently_unavailable("hook-enable-failed-rollback-unconfirmed");
                    return;
                }
                report_retryable_failure("hook-enable-failed-rollback-confirmed");
                return;
            }
        }

        m_overlay_writer_hooks = std::move(pending_hooks);
        m_overlay_writer_image_base.store(image.base, std::memory_order_release);
        m_overlay_writer_active.store(true, std::memory_order_release);
        if (m_overlay_writer_diagnostics_enabled.load(std::memory_order_acquire)) {
            spdlog::info("[RE4XeSS][OverlayWriter] armed attempts={} imageBase=0x{:x} imageSize=0x{:x} imageChecksum=0x{:x} imageIdentity=RE4-1.5.9.0-verified overlay=0x{:x} slot=0x{:x} slotOffset=0x90 liveVtable=0x{:x} liveVcall40=0x{:x} nativeMethodRva=0x44af030 hooks=8 stages=pre/post exactBytesValidated=true diagnosticCapture=64 pre-Overlay frames per mode transition transactionObservation=generation-scoped previousFailure={}",
                install_attempt, image.base, image.size, image.checksum, overlay_address, slot_address,
                overlay_vtable, writer_method,
                m_overlay_writer_last_install_failure != nullptr ? m_overlay_writer_last_install_failure : "none");
        }
    }

    template <size_t HookCount>
    static bool rollback_overlay_writer_hooks(
        std::array<safetyhook::MidHook, HookCount>& hooks,
        uintptr_t image_base) noexcept {
        constexpr std::array<uint32_t, 8> hook_rvas{
            0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
            0x44AF460, 0x44AF467, 0x44AF478, 0x44AF47F,
        };
        constexpr std::array<size_t, 8> expected_spans{ 7, 5, 7, 5, 7, 5, 7, 5 };
        constexpr std::array<std::array<uint8_t, 7>, 8> expected_bytes{{
            { 0x4C, 0x89, 0xB6, 0x90, 0x00, 0x00, 0x00 },
            { 0x48, 0x85, 0xC9, 0x74, 0x05 },
            { 0x48, 0x89, 0x9E, 0x90, 0x00, 0x00, 0x00 },
            { 0x48, 0x85, 0xC9, 0x74, 0x05 },
            { 0x4C, 0x89, 0xB6, 0x90, 0x00, 0x00, 0x00 },
            { 0x48, 0x85, 0xC9, 0x74, 0x05 },
            { 0x48, 0x89, 0xBE, 0x90, 0x00, 0x00, 0x00 },
            { 0x48, 0x85, 0xC9, 0x74, 0x05 },
        }};
        static_assert(HookCount == expected_spans.size());
        bool rollback_confirmed = true;
        for (auto& hook : hooks) {
            if (hook && !hook.disable()) {
                rollback_confirmed = false;
            }
        }
        for (size_t index = 0; index < hooks.size(); ++index) {
            const auto& hook = hooks[index];
            std::array<uint8_t, 7> current_bytes{};
            if (!read_memory(image_base + hook_rvas[index], current_bytes.data(), expected_spans[index]) ||
                !std::equal(expected_bytes[index].begin(),
                    expected_bytes[index].begin() + expected_spans[index], current_bytes.begin())) {
                rollback_confirmed = false;
            }
            if (hook) {
                const auto& original_bytes = hook.original_bytes();
                if (hook.enabled() || original_bytes.size() != expected_spans[index] ||
                    !std::equal(original_bytes.begin(), original_bytes.end(), expected_bytes[index].begin())) {
                    rollback_confirmed = false;
                }
            }
        }
        return rollback_confirmed;
    }

private:
    struct OverlayWriterPending {
        uint64_t event{};
        uint64_t store_sequence{};
        uint64_t invocation_id{};
        uintptr_t invocation_stack_pointer{};
        uintptr_t overlay{};
        uintptr_t slot{};
        uintptr_t prior{};
        uintptr_t loaded_previous{};
        uintptr_t incoming{};
        uintptr_t caller_return{};
        uintptr_t caller_rva{};
        uint64_t generation{};
        uint64_t device_reset_generation{};
        int32_t mode_token{};
        uint32_t thread_id{};
        bool clear_store{};
        bool tracked_overlay{};
        bool transaction_attached{};
        bool prior_readable{};
        bool caller_readable{};
        bool diagnostic_log{};
        bool valid{};
    };

    struct OverlayWriterCallbackGuard {
        explicit OverlayWriterCallbackGuard(std::atomic<uint32_t>& in_flight) noexcept
            : counter{ in_flight } {
            counter.fetch_add(1, std::memory_order_acq_rel);
        }
        ~OverlayWriterCallbackGuard() {
            counter.fetch_sub(1, std::memory_order_acq_rel);
        }
        std::atomic<uint32_t>& counter;
    };

    struct OverlayWriterTransactionPending {
        uint64_t invocation_id{};
        uintptr_t invocation_stack_pointer{};
        uintptr_t overlay{};
        uintptr_t slot{};
        uintptr_t cleared_state{};
        uintptr_t clear_state_after{};
        uintptr_t replacement_previous_state{};
        uintptr_t incoming_state{};
        uintptr_t replacement_state_after{};
        uint64_t generation{};
        uint64_t device_reset_generation{};
        uint64_t clear_pre_sequence{};
        uint64_t clear_post_sequence{};
        uint64_t replacement_pre_sequence{};
        uint64_t replacement_post_sequence{};
        int32_t mode_token{};
        uint32_t writer_thread_id{};
        uint32_t clear_pre_write_rva{};
        uint32_t clear_post_write_rva{};
        uint32_t replacement_pre_write_rva{};
        uint32_t replacement_post_write_rva{};
        size_t branch_index{};
        bool clear_write_confirmed{};
        bool replacement_write_confirmed{};
        bool valid{};
    };

    // The two verified store sites in each RE4 branch share the original RSP and thread
    // across the clear -> replacement pair. Nested calls have a distinct stack frame;
    // using this bounded TLS table avoids confusing their writes with the outer call.
    static OverlayWriterTransactionPending* find_overlay_writer_transaction(
        size_t branch_index,
        uintptr_t invocation_stack_pointer,
        bool create) noexcept {
        if (branch_index >= OVERLAY_WRITER_BRANCH_COUNT || invocation_stack_pointer == 0) {
            return nullptr;
        }
        const auto first = branch_index * MAX_OVERLAY_WRITER_NESTING;
        OverlayWriterTransactionPending* free_slot{};
        for (size_t index = first; index < first + MAX_OVERLAY_WRITER_NESTING; ++index) {
            auto& candidate = s_pending_overlay_writer_transactions[index];
            if (candidate.valid && candidate.invocation_stack_pointer == invocation_stack_pointer) {
                return &candidate;
            }
            if (!candidate.valid && free_slot == nullptr) {
                free_slot = &candidate;
            }
        }
        return create ? free_slot : nullptr;
    }

    static OverlayWriterTransactionPending* find_overlay_writer_transaction(
        size_t branch_index,
        uintptr_t invocation_stack_pointer,
        uint64_t invocation_id) noexcept {
        auto* candidate = find_overlay_writer_transaction(branch_index, invocation_stack_pointer, false);
        return candidate != nullptr && candidate->invocation_id == invocation_id ? candidate : nullptr;
    }

    template <size_t HookIndex>
    static void overlay_writer_callback(safetyhook::Context& context) {
        auto& probe = instance();
        if (!probe.m_overlay_writer_enabled.load(std::memory_order_acquire) ||
            !probe.m_overlay_writer_active.load(std::memory_order_acquire) ||
            !probe.m_overlay_writer_transaction_enabled.load(std::memory_order_acquire)) {
            return;
        }
        OverlayWriterCallbackGuard callback_guard{ probe.m_overlay_writer_callback_in_flight };
        constexpr size_t store_index = HookIndex / 2;
        constexpr bool after_store = (HookIndex % 2) != 0;
        constexpr size_t branch_index = store_index / 2;
        constexpr bool clear_store = (store_index % 2) == 0;
        auto& pending = s_pending_overlay_writer_stores[store_index];
        const auto tracked_overlay =
            context.rsi == probe.m_overlay_writer_overlay.load(std::memory_order_acquire);
        const auto slot = context.rsi + RE4_TARGET_STATE_SLOT_OFFSET;
        const auto image_base = probe.m_overlay_writer_image_base.load(std::memory_order_acquire);
        if constexpr (!after_store) {
            pending = {};
            const auto event = probe.m_overlay_writer_events.fetch_add(1, std::memory_order_acq_rel);
            const auto store_sequence = probe.m_overlay_writer_store_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
            const bool diagnostic_log =
                tracked_overlay &&
                probe.m_overlay_writer_diagnostic_capture.load(std::memory_order_acquire) &&
                probe.m_overlay_writer_diagnostics_enabled.load(std::memory_order_acquire) &&
                event < MAX_OVERLAY_WRITER_DIAGNOSTIC_EVENTS;
            uintptr_t prior{};
            const bool prior_readable = read_private_memory(slot, &prior, sizeof(prior));
            const auto incoming = overlay_writer_source_register(store_index, context);
            const auto generation = probe.m_overlay_writer_generation.load(std::memory_order_acquire);
            const auto device_reset_generation =
                probe.m_overlay_writer_device_reset_generation.load(std::memory_order_acquire);
            const auto mode_token = probe.m_overlay_writer_mode.load(std::memory_order_acquire);
            uint64_t invocation_id{};
            auto* transaction = tracked_overlay
                ? find_overlay_writer_transaction(branch_index, context.rsp, clear_store)
                : nullptr;
            if constexpr (clear_store) {
                if (transaction != nullptr) {
                    *transaction = {};
                    invocation_id = probe.m_overlay_writer_next_invocation_id.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                    transaction->invocation_id = invocation_id;
                    transaction->invocation_stack_pointer = context.rsp;
                    transaction->overlay = context.rsi;
                    transaction->slot = slot;
                    transaction->cleared_state = prior;
                    transaction->generation = generation;
                    transaction->device_reset_generation = device_reset_generation;
                    transaction->mode_token = mode_token;
                    transaction->writer_thread_id = GetCurrentThreadId();
                    transaction->clear_pre_sequence = store_sequence;
                    transaction->clear_pre_write_rva = OVERLAY_WRITER_PRE_RVAS[store_index];
                    transaction->branch_index = branch_index;
                    transaction->valid = true;
                } else {
                    probe.m_overlay_writer_transaction_overflow.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                if (transaction != nullptr && transaction->valid && transaction->clear_write_confirmed &&
                    transaction->overlay == context.rsi && transaction->slot == slot &&
                    transaction->writer_thread_id == GetCurrentThreadId() &&
                    transaction->generation == generation &&
                    transaction->device_reset_generation == device_reset_generation &&
                    transaction->mode_token == mode_token) {
                    invocation_id = transaction->invocation_id;
                    transaction->replacement_previous_state = prior;
                    transaction->incoming_state = incoming;
                    transaction->replacement_pre_sequence = store_sequence;
                    transaction->replacement_pre_write_rva = OVERLAY_WRITER_PRE_RVAS[store_index];
                    transaction->replacement_write_confirmed = false;
                } else {
                    if (transaction != nullptr) {
                        transaction->valid = false;
                    }
                    transaction = nullptr;
                }
            }
            uintptr_t caller_return{};
            const bool caller_readable = context.rsp <= UINTPTR_MAX - 0x138 &&
                read_private_memory(context.rsp + 0x138, &caller_return, sizeof(caller_return));
            const auto caller_rva = caller_readable && caller_return >= image_base &&
                caller_return < image_base + EXPECTED_IMAGE_SIZE ? caller_return - image_base : 0;
            pending.event = event + 1;
            pending.store_sequence = store_sequence;
            pending.invocation_id = invocation_id;
            pending.invocation_stack_pointer = context.rsp;
            pending.overlay = context.rsi;
            pending.slot = slot;
            pending.prior = prior;
            pending.loaded_previous = context.rcx;
            pending.incoming = incoming;
            pending.caller_return = caller_return;
            pending.caller_rva = caller_rva;
            pending.generation = generation;
            pending.device_reset_generation = device_reset_generation;
            pending.mode_token = mode_token;
            pending.thread_id = GetCurrentThreadId();
            pending.clear_store = clear_store;
            pending.tracked_overlay = tracked_overlay;
            pending.transaction_attached = transaction != nullptr;
            pending.prior_readable = prior_readable;
            pending.caller_readable = caller_readable;
            pending.diagnostic_log = diagnostic_log;
            pending.valid = true;
            const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (diagnostic_log) {
                spdlog::info("[RE4XeSS][OverlayWriter] stage=pre-write timestampUs={} event={} storeSequence={} invocationId={} invocationFrameRsp=0x{:x} transactionStage={} transactionAttached={} tid={} module=re4.exe methodRva=0x44af030 siteRva=0x{:x} destination=0x{:x} destinationValidated=true overlay=0x{:x} slot=0x{:x} prior=0x{:x} loadedPrevious=0x{:x} priorReadable={} incoming=0x{:x} sourceRegister={} callerReturn=0x{:x} callerRva=0x{:x} callerReadable={} controlGeneration={} modeToken={} diagnosticFramesSinceTransition={}",
                    static_cast<long long>(timestamp_us), event + 1,
                    static_cast<unsigned long long>(store_sequence),
                    static_cast<unsigned long long>(invocation_id), context.rsp,
                    clear_store ? "clear" : "replacement", pending.transaction_attached, pending.thread_id,
                    OVERLAY_WRITER_PRE_RVAS[store_index], slot, context.rsi, slot, prior, context.rcx,
                    prior_readable, incoming, overlay_writer_source_register_name(store_index),
                    caller_return, caller_rva, caller_readable, pending.generation, pending.mode_token,
                    probe.m_overlay_writer_frames.load(std::memory_order_acquire));
            }
        } else {
            const auto post_store_sequence =
                probe.m_overlay_writer_store_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (!pending.valid || pending.overlay != context.rsi || pending.slot != slot ||
                pending.thread_id != GetCurrentThreadId() ||
                pending.invocation_stack_pointer != context.rsp) {
                return;
            }
            uintptr_t observed{};
            const bool observed_readable = read_private_memory(slot, &observed, sizeof(observed));
            const bool generation_still_current =
                pending.generation == probe.m_overlay_writer_generation.load(std::memory_order_acquire) &&
                pending.device_reset_generation ==
                    probe.m_overlay_writer_device_reset_generation.load(std::memory_order_acquire) &&
                pending.mode_token == probe.m_overlay_writer_mode.load(std::memory_order_acquire);
            const bool confirmed = observed_readable && observed == pending.incoming &&
                pending.prior_readable && pending.loaded_previous == pending.prior &&
                generation_still_current;
            if (pending.tracked_overlay && confirmed) {
                probe.m_overlay_writer_confirmed_writes.fetch_add(1, std::memory_order_acq_rel);
            } else if (pending.tracked_overlay) {
                probe.m_overlay_writer_unconfirmed_writes.fetch_add(1, std::memory_order_acq_rel);
            }
            auto* transaction_candidate = pending.tracked_overlay
                ? find_overlay_writer_transaction(branch_index, pending.invocation_stack_pointer, false)
                : nullptr;
            auto* transaction = pending.transaction_attached && transaction_candidate != nullptr &&
                transaction_candidate->invocation_id == pending.invocation_id
                ? transaction_candidate
                : nullptr;
            if (pending.transaction_attached && transaction_candidate != nullptr && transaction == nullptr) {
                transaction_candidate->valid = false;
            }
            if (pending.clear_store) {
                if (transaction != nullptr) {
                    const bool transaction_matches = transaction->valid &&
                        transaction->invocation_id == pending.invocation_id &&
                        transaction->invocation_stack_pointer == pending.invocation_stack_pointer &&
                        transaction->overlay == pending.overlay && transaction->slot == pending.slot &&
                        transaction->writer_thread_id == pending.thread_id &&
                        transaction->generation == pending.generation &&
                        transaction->device_reset_generation == pending.device_reset_generation &&
                        transaction->mode_token == pending.mode_token;
                    transaction->clear_state_after = observed;
                    transaction->clear_post_sequence = post_store_sequence;
                    transaction->clear_post_write_rva = OVERLAY_WRITER_POST_RVAS[store_index];
                    transaction->clear_write_confirmed = transaction_matches && confirmed &&
                        observed == 0 && pending.incoming == 0;
                    if (!transaction->clear_write_confirmed) {
                        transaction->valid = false;
                    }
                }
            } else if (transaction != nullptr) {
                const bool transaction_matches = transaction->valid &&
                    transaction->invocation_id == pending.invocation_id &&
                    transaction->invocation_stack_pointer == pending.invocation_stack_pointer &&
                    transaction->overlay == pending.overlay && transaction->slot == pending.slot &&
                    transaction->writer_thread_id == pending.thread_id &&
                    transaction->generation == pending.generation &&
                    transaction->device_reset_generation == pending.device_reset_generation &&
                    transaction->mode_token == pending.mode_token &&
                    transaction->branch_index == branch_index;
                transaction->replacement_state_after = observed;
                transaction->replacement_post_sequence = post_store_sequence;
                transaction->replacement_post_write_rva = OVERLAY_WRITER_POST_RVAS[store_index];
                transaction->replacement_write_confirmed = transaction_matches && confirmed &&
                    transaction->incoming_state != 0 &&
                    transaction->replacement_previous_state == 0 && observed == transaction->incoming_state;
                if (transaction->replacement_write_confirmed && transaction->clear_write_confirmed) {
                    probe.publish_overlay_writer_witness(*transaction);
                    transaction->valid = false;
                } else {
                    transaction->valid = false;
                }
            }
            const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (pending.diagnostic_log) {
                spdlog::info("[RE4XeSS][OverlayWriter] stage=post-write timestampUs={} event={} storeSequence={} invocationId={} invocationFrameRsp=0x{:x} transactionStage={} tid={} module=re4.exe methodRva=0x44af030 siteRva=0x{:x} destination=0x{:x} destinationValidated=true overlay=0x{:x} slot=0x{:x} prior=0x{:x} loadedPrevious=0x{:x} incoming=0x{:x} observedAfter=0x{:x} slotReadValid={} confirmedWrite={} generationStillCurrent={} transactionComplete={} preSiteRva=0x{:x} callerReturn=0x{:x} callerRva=0x{:x} controlGeneration={} modeToken={}",
                    static_cast<long long>(timestamp_us), pending.event,
                    static_cast<unsigned long long>(pending.store_sequence),
                    static_cast<unsigned long long>(pending.invocation_id), pending.invocation_stack_pointer,
                    pending.clear_store ? "clear" : "replacement", GetCurrentThreadId(),
                    OVERLAY_WRITER_POST_RVAS[store_index], slot, context.rsi, slot, pending.prior,
                    pending.loaded_previous, pending.incoming, observed, observed_readable, confirmed,
                    generation_still_current,
                    !pending.clear_store && transaction == nullptr ? false :
                        !pending.clear_store && transaction != nullptr &&
                        transaction->replacement_write_confirmed && transaction->clear_write_confirmed,
                    OVERLAY_WRITER_PRE_RVAS[store_index], pending.caller_return, pending.caller_rva,
                    pending.generation, pending.mode_token);
            }
            pending.valid = false;
        }
    }

    static uintptr_t overlay_writer_source_register(size_t store_index, safetyhook::Context& context) noexcept {
        switch (store_index) {
        case 0: return context.r14;
        case 1: return context.rbx;
        case 2: return context.r14;
        case 3: return context.rdi;
        default: return 0;
        }
    }

    static const char* overlay_writer_source_register_name(size_t store_index) noexcept {
        switch (store_index) {
        case 0: return "r14";
        case 1: return "rbx";
        case 2: return "r14";
        case 3: return "rdi";
        default: return "unknown";
        }
    }

    void publish_overlay_writer_witness(
        const OverlayWriterTransactionPending& transaction) noexcept {
        if (transaction.branch_index >= OVERLAY_WRITER_BRANCH_COUNT ||
            !transaction.valid || !transaction.clear_write_confirmed ||
            !transaction.replacement_write_confirmed) {
            return;
        }

        std::lock_guard lock{ m_overlay_writer_transition_mutex };
        if (transaction.generation != m_overlay_writer_generation.load(std::memory_order_acquire) ||
            transaction.device_reset_generation != m_overlay_writer_device_reset_generation.load(std::memory_order_acquire) ||
            transaction.mode_token != m_overlay_writer_mode.load(std::memory_order_acquire)) {
            return;
        }
        re4_xess::EngineWriterWitness witness{};
        witness.sequence = transaction.replacement_post_sequence;
        witness.invocation_id = transaction.invocation_id;
        witness.invocation_stack_pointer = transaction.invocation_stack_pointer;
        witness.clear_pre_sequence = transaction.clear_pre_sequence;
        witness.clear_post_sequence = transaction.clear_post_sequence;
        witness.replacement_pre_sequence = transaction.replacement_pre_sequence;
        witness.replacement_post_sequence = transaction.replacement_post_sequence;
        witness.current_store_sequence = transaction.replacement_post_sequence;
        witness.control_generation = transaction.generation;
        witness.device_reset_generation = transaction.device_reset_generation;
        witness.mode_token = transaction.mode_token;
        witness.writer_thread_id = transaction.writer_thread_id;
        witness.overlay = transaction.overlay;
        witness.slot = transaction.slot;
        witness.cleared_state = transaction.cleared_state;
        witness.clear_state_after = transaction.clear_state_after;
        witness.replacement_previous_state = transaction.replacement_previous_state;
        witness.incoming_state = transaction.incoming_state;
        witness.replacement_state_after = transaction.replacement_state_after;
        witness.method_rva = re4_xess::OVERLAY_WRITER_METHOD_RVA;
        witness.clear_pre_write_rva = transaction.clear_pre_write_rva;
        witness.clear_post_write_rva = transaction.clear_post_write_rva;
        witness.replacement_pre_write_rva = transaction.replacement_pre_write_rva;
        witness.replacement_post_write_rva = transaction.replacement_post_write_rva;
        witness.destination_validated = true;
        witness.same_invocation = true;
        witness.clear_write_confirmed = true;
        witness.replacement_write_confirmed = true;
        witness.re4_image_identity_verified = true;

        if (m_overlay_writer_witness_history.publish(witness)) {
            m_overlay_writer_confirmed_transactions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    struct ImageInfo {
        uintptr_t base{};
        uint32_t size{};
        uint32_t checksum{};
        uint32_t section_table_rva{};
        IMAGE_NT_HEADERS64 nt{};
        bool valid{};
    };

    struct CreatorObservation {
        std::atomic<bool> ready{};
        uint64_t id{};
        DWORD thread_id{};
        uintptr_t caller_return_rva{};
        uintptr_t caller_callsite_rva{};
        uintptr_t rcx{};
        uintptr_t rdx{};
        uintptr_t r8{};
        uintptr_t r9{};
        uintptr_t rdi{};
        std::atomic<uintptr_t> allocation_result{};
        std::atomic<uintptr_t> initialized_object{};
        std::atomic<uintptr_t> constructed_object{};
        std::atomic<uintptr_t> returned_object{};
        std::atomic<uintptr_t> object_vtable{};
        std::atomic<int32_t> object_ref_count{};
        std::atomic<uint32_t> object_render_frame{};
        std::atomic<uintptr_t> object_rtvs{};
        std::atomic<uintptr_t> object_dsv{};
        std::atomic<uint32_t> object_num_rtv{};
        std::atomic<float> object_rect_left{};
        std::atomic<float> object_rect_top{};
        std::atomic<float> object_rect_right{};
        std::atomic<float> object_rect_bottom{};
        std::atomic<uint32_t> object_flag{};
        std::atomic<uintptr_t> object_rtv0{};
        std::atomic<bool> object_rtv_valid{};
        std::atomic<uint32_t> object_rtv_format{};
        std::atomic<uint32_t> object_rtv_dimension{};
        std::atomic<uintptr_t> object_texture{};
        std::atomic<bool> object_texture_header_valid{};
        std::atomic<uintptr_t> object_texture_vtable{};
        std::atomic<int32_t> object_texture_ref_count{};
        std::atomic<uint32_t> object_texture_render_frame{};
        std::atomic<bool> object_target_state_like{};
    };

    enum class CreatorHookPoint : size_t {
        FunctionEntry,
        AllocationReturned,
        InitializerReturned,
        SuccessReturn,
        AllocationFailureReturn,
        Count,
    };

    struct CallArguments {
        uintptr_t rcx{};
        uintptr_t rdx{};
        uintptr_t r8{};
        uintptr_t r9{};
        uint8_t valid_mask{};
    };

    struct PendingObservation {
        bool active{};
        size_t site{};
        uintptr_t stack_pointer{};
        uint64_t id{};
        CallArguments arguments{};
        uintptr_t callee{};
    };

    struct PendingProvenanceWrite {
        uint64_t id{};
        uint64_t frame_id{};
        uintptr_t slot{};
        uintptr_t previous_value{};
        uintptr_t incoming_value{};
        uint32_t writer_site_rva{};
        uint32_t frames_waited{};
    };

    struct EarlyProvenanceObservation {
        uint64_t id{};
        uint64_t frame_id{};
        uint32_t writer_site_rva{};
        uint32_t caller_return_rva{};
        uint32_t caller_callsite_rva{};
        DWORD thread_id{};
        uintptr_t receiver{};
        uintptr_t slot{};
        uintptr_t previous_value{};
        uintptr_t incoming_value{};
        uintptr_t source_owner{};
        uintptr_t source_slot{};
        uintptr_t source_value{};
        uintptr_t caller_return{};
        std::array<uintptr_t, 5> registers{};
        std::array<uintptr_t, 4> stack_arguments{};
        int32_t receiver_ref_count{};
        bool previous_readable{};
        bool source_slot_readable{};
        bool caller_return_readable{};
        bool stack_arguments_readable{};
        bool receiver_header_readable{};
        bool direct_transfer_route{};
    };

    struct EarlyProvenanceEntry {
        std::atomic<bool> ready{};
        EarlyProvenanceObservation observation{};
    };

    struct TargetStateLayoutSnapshot {
        uintptr_t vtable{};
        int32_t ref_count{};
        uint32_t render_frame{};
        uintptr_t padding{};
        uintptr_t rtvs{};
        uintptr_t dsv{};
        uint32_t num_rtv{};
        float rect_left{};
        float rect_top{};
        float rect_right{};
        float rect_bottom{};
        uint32_t flag{};
    };

    struct RenderResourceHeaderSnapshot {
        uintptr_t vtable{};
        int32_t ref_count{};
        uint32_t render_frame{};
        uintptr_t padding{};
    };

    struct RenderTargetViewSnapshot {
        RenderResourceHeaderSnapshot header{};
        uint32_t format{};
        uint32_t dimension{};
    };

    static_assert(sizeof(TargetStateLayoutSnapshot) == 0x40);
    static_assert(offsetof(TargetStateLayoutSnapshot, rtvs) == 0x18);
    static_assert(offsetof(TargetStateLayoutSnapshot, dsv) == 0x20);
    static_assert(offsetof(TargetStateLayoutSnapshot, num_rtv) == 0x28);
    static_assert(offsetof(TargetStateLayoutSnapshot, rect_left) == 0x2C);
    static_assert(offsetof(TargetStateLayoutSnapshot, flag) == 0x3C);
    static_assert(sizeof(RenderResourceHeaderSnapshot) == 0x18);
    static_assert(sizeof(RenderTargetViewSnapshot) == 0x20);
    static_assert(offsetof(RenderTargetViewSnapshot, format) == 0x18);

    static constexpr uint32_t EXPECTED_IMAGE_SIZE = 0x0E405000;
    static constexpr uint32_t EXPECTED_IMAGE_CHECKSUM = 0x0DEE3479;
    static constexpr uint32_t TARGET_STATE_VTABLE_RVA = 0x7B1C148;
    static constexpr uint32_t RE4_PROVIDER_CALLEE_RVA = 0x78F42D0;
    static constexpr uintptr_t RE4_RENDER_RESOURCE_SIZE = 0x18;
    static constexpr uint32_t RETIRED_RE4_TARGET_STATE_WRITER_RVA = 0x4597A0;
    static constexpr uint32_t RE4_TARGET_STATE_CREATOR_RVA = 0x47D2180;
    static constexpr uint32_t RE4_TARGET_STATE_CREATOR_END_RVA = 0x47D21D2;
    static constexpr uint32_t RE4_TARGET_STATE_SLOT_OFFSET = 0x90;
    static constexpr uint32_t RE4_TARGET_STATE_SOURCE_OWNER_RVA = 0xD6974F0;
    static constexpr size_t PROVENANCE_HOOK_COUNT = 3;
    static constexpr uint32_t MAX_PROVENANCE_WRITES = 8;
    static constexpr uint32_t MAX_PROVENANCE_FRAMES = 1800;
    static constexpr uint32_t MAX_PROVENANCE_PENDING_FRAMES = 4;
    static constexpr uint32_t MAX_PROVENANCE_PENDING_GRACE = 2;
    static constexpr size_t MAX_EARLY_PROVENANCE_WRITES = 32;
    static constexpr uint32_t MAX_TARGET_STATE_RTVS = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
    static constexpr size_t SITE_COUNT = 4;
    // Isolate the nested owner vcall so its ABI and return can be measured without other probe hooks.
    static constexpr size_t ISOLATED_SITE_INDEX = 1;
    static constexpr uint32_t MAX_PENDING_PRESENT_GRACE = 2;
    static constexpr size_t MAX_CAPTURED_CALLS_PER_SITE = 4;
    static constexpr size_t MAX_CREATOR_OBSERVATIONS = 16;
    static constexpr size_t MAX_CREATOR_ACTIVE_DEPTH = 8;
    static constexpr uint32_t MAX_CREATOR_CAPTURE_FRAMES = 1800;
    static constexpr size_t MAX_CAPTURED_CALLS = SITE_COUNT * MAX_CAPTURED_CALLS_PER_SITE;
    static constexpr size_t HOOK_COUNT = SITE_COUNT * 2;
    static constexpr size_t OVERLAY_WRITER_STORE_COUNT = 4;
    static constexpr size_t OVERLAY_WRITER_BRANCH_COUNT = OVERLAY_WRITER_STORE_COUNT / 2;
    static constexpr size_t MAX_OVERLAY_WRITER_NESTING = 8;
    static constexpr size_t OVERLAY_WRITER_HOOK_COUNT = OVERLAY_WRITER_STORE_COUNT * 2;
    static constexpr uint64_t MAX_OVERLAY_WRITER_DIAGNOSTIC_EVENTS = 16;
    static constexpr std::array<uint32_t, OVERLAY_WRITER_STORE_COUNT> OVERLAY_WRITER_PRE_RVAS{
        0x44AF161, 0x44AF179, 0x44AF460, 0x44AF478
    };
    static constexpr std::array<uint32_t, OVERLAY_WRITER_STORE_COUNT> OVERLAY_WRITER_POST_RVAS{
        0x44AF168, 0x44AF180, 0x44AF467, 0x44AF47F
    };
    static constexpr std::array<uint32_t, OVERLAY_WRITER_HOOK_COUNT> OVERLAY_WRITER_HOOK_RVAS{
        0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
        0x44AF460, 0x44AF467, 0x44AF478, 0x44AF47F
    };
    static constexpr std::array<size_t, OVERLAY_WRITER_HOOK_COUNT> OVERLAY_WRITER_HOOK_SPANS{
        7, 5, 7, 5, 7, 5, 7, 5
    };
    static constexpr std::array<uint32_t, SITE_COUNT> PRE_HOOK_RVAS{
        0x447AF53,
        0x47212A0,
        0x44C7A21,
        0x47D0FC8,
    };
    static constexpr std::array<uint32_t, SITE_COUNT> CALLSITE_RVAS{
        0x447AF5A,
        0x47212A6,
        0x44C7A27,
        0x47D0FCF,
    };
    static constexpr std::array<uint32_t, SITE_COUNT> RETURN_RVAS{
        0x447AF5F,
        0x47212A9,
        0x44C7A2A,
        0x47D0FD5,
    };
    static constexpr std::array<const char*, SITE_COUNT> SITE_NAMES{
        "render-target-view factory call",
        "RTV owner vcall+0x40 A",
        "RTV owner vcall+0x40 B",
        "render-resource vcall+0xA0",
    };

    std::atomic<bool> m_attempted{};
    std::atomic<bool> m_active{};
    std::atomic<bool> m_disarm_started{};
    std::atomic<bool> m_capture_started{};
    std::atomic<bool> m_capture_stopped{};
    std::atomic<uint32_t> m_calls_captured{};
    std::atomic<uint32_t> m_returns_captured{};
    std::atomic<uint32_t> m_pending_call_count{};
    std::atomic<uint32_t> m_present_grace_count{};
    std::atomic<uint64_t> m_next_call_id{};
    std::array<std::atomic<uint32_t>, SITE_COUNT> m_site_call_counts{};
    std::array<std::atomic<uint32_t>, SITE_COUNT> m_site_return_counts{};
    std::array<std::atomic<uint32_t>, SITE_COUNT> m_return_hook_entries{};
    std::array<std::atomic<uint32_t>, SITE_COUNT> m_unmatched_returns{};
    std::atomic<uintptr_t> m_expected_vtable{};
    std::atomic<bool> m_vtable_discovery_done{};
    std::atomic<bool> m_vtable_discovery_complete{};
    std::atomic<bool> m_image_identity_valid{};
    std::atomic<bool> m_live_anchor_logged{};
    std::atomic<bool> m_live_anchor_invalid_logged{};
    std::atomic<bool> m_live_anchor_trusted{};
    std::atomic<bool> m_overlay_writer_enabled{};
    std::atomic<bool> m_overlay_writer_diagnostics_enabled{};
    std::atomic<bool> m_overlay_writer_active{};
    std::atomic<bool> m_overlay_writer_permanently_unavailable{};
    std::mutex m_overlay_writer_install_mutex{};
    uint32_t m_overlay_writer_install_attempts{};
    const char* m_overlay_writer_last_install_failure{};
    std::atomic<bool> m_overlay_writer_transaction_enabled{};
    std::atomic<bool> m_overlay_writer_diagnostic_capture{};
    std::atomic<uintptr_t> m_overlay_writer_overlay{};
    std::atomic<uintptr_t> m_overlay_writer_image_base{};
    std::atomic<uint64_t> m_overlay_writer_generation{};
    std::atomic<uint64_t> m_overlay_writer_device_reset_generation{};
    std::atomic<int32_t> m_overlay_writer_mode{};
    std::atomic<uint32_t> m_overlay_writer_frames{};
    std::atomic<uint64_t> m_overlay_writer_events{};
    std::atomic<uint64_t> m_overlay_writer_store_sequence{};
    std::atomic<uint32_t> m_overlay_writer_callback_in_flight{};
    std::atomic<uint64_t> m_overlay_writer_next_invocation_id{};
    std::atomic<uint64_t> m_overlay_writer_transaction_overflow{};
    std::atomic<uint64_t> m_overlay_writer_confirmed_writes{};
    std::atomic<uint64_t> m_overlay_writer_unconfirmed_writes{};
    std::atomic<uint64_t> m_overlay_writer_confirmed_transactions{};
    mutable std::mutex m_overlay_writer_transition_mutex{};
    re4_xess::EngineWriterWitnessHistory m_overlay_writer_witness_history{};
    std::array<safetyhook::MidHook, OVERLAY_WRITER_HOOK_COUNT> m_overlay_writer_hooks{};
    std::atomic<uintptr_t> m_provenance_overlay_object{};
    std::atomic<uintptr_t> m_provenance_overlay_slot{};
    std::atomic<bool> m_provenance_slot_invalid_logged{};
    std::atomic<bool> m_provenance_attempted{};
    std::atomic<bool> m_provenance_active{};
    std::atomic<bool> m_provenance_disarm_started{};
    std::atomic<uint32_t> m_provenance_frames{};
    std::atomic<uint32_t> m_provenance_write_count{};
    std::atomic<uint32_t> m_provenance_confirmed_count{};
    std::atomic<uint32_t> m_provenance_pending_grace_frames{};
    std::atomic<bool> m_provenance_pending_overflow_logged{};
    std::atomic<uint64_t> m_provenance_next_id{};
    std::atomic<int> m_provenance_pending_state{};
    std::atomic<uint32_t> m_early_provenance_count{};
    std::atomic<uint32_t> m_early_provenance_reported_count{};
    std::atomic<uint32_t> m_early_provenance_correlated_count{};
    std::atomic<bool> m_early_provenance_overflow{};
    std::atomic<bool> m_early_provenance_overflow_logged{};
    std::array<EarlyProvenanceEntry, MAX_EARLY_PROVENANCE_WRITES> m_early_provenance_observations{};
    PendingProvenanceWrite m_pending_provenance_write{};
    std::array<safetyhook::MidHook, PROVENANCE_HOOK_COUNT> m_provenance_hooks{};
    uintptr_t m_image_base{};
    uintptr_t m_image_end{};
    uint64_t m_frame_id{};
    DWORD m_arm_thread_id{};
    uintptr_t m_overlay_state{};
    uintptr_t m_overlay_desc{};
    uintptr_t m_overlay_rtv_array{};
    uintptr_t m_overlay_rtv{};
    uintptr_t m_semantic_color{};
    TargetStateLayoutSnapshot m_overlay_snapshot{};
    bool m_overlay_snapshot_valid{};
    bool m_overlay_snapshot_attempted{};
    std::array<safetyhook::MidHook, HOOK_COUNT> m_hooks{};
    std::atomic<bool> m_creator_attempted{};
    std::atomic<bool> m_creator_active{};
    std::atomic<uint32_t> m_creator_call_count{};
    std::atomic<uint32_t> m_creator_ready_count{};
    std::atomic<uint32_t> m_creator_frames{};
    std::atomic<bool> m_creator_disarm_started{};
    std::array<safetyhook::MidHook, 5> m_creator_hooks{};
    std::array<CreatorObservation, MAX_CREATOR_OBSERVATIONS> m_creator_observations{};
    uintptr_t m_last_creator_correlation_state{};
    uint32_t m_last_creator_correlation_count{ UINT32_MAX };
    static inline thread_local std::array<size_t, MAX_CREATOR_ACTIVE_DEPTH> s_creator_active_calls{};
    static inline thread_local size_t s_creator_active_depth{};
    inline static thread_local std::array<PendingObservation, MAX_CAPTURED_CALLS> s_pending_observations{};
    inline static thread_local std::array<OverlayWriterPending, OVERLAY_WRITER_STORE_COUNT> s_pending_overlay_writer_stores{};
    inline static thread_local std::array<OverlayWriterTransactionPending,
        OVERLAY_WRITER_BRANCH_COUNT * MAX_OVERLAY_WRITER_NESTING> s_pending_overlay_writer_transactions{};

    static bool read_memory(uintptr_t address, void* destination, size_t size) noexcept {
        if (address == 0 || destination == nullptr || size == 0 || address > UINTPTR_MAX - size) {
            return false;
        }
        SIZE_T bytes_read{};
        return ReadProcessMemory(
                   GetCurrentProcess(),
                   reinterpret_cast<const void*>(address),
                   destination,
                   size,
                   &bytes_read) != FALSE &&
            bytes_read == size;
    }

    static bool is_private_readable(uintptr_t address, size_t size) noexcept {
        if (address == 0 || size == 0 || address > UINTPTR_MAX - size) {
            return false;
        }

        const auto end = address + size;
        auto cursor = address;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) ||
                memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
                (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
                return false;
            }

            const auto protection = memory.Protect & 0xFF;
            if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
                protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
                protection != PAGE_EXECUTE_WRITECOPY) {
                return false;
            }

            const auto region_end = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            if (region_end <= cursor) {
                return false;
            }
            cursor = std::min(end, region_end);
        }
        return true;
    }

    static bool read_private_memory(uintptr_t address, void* destination, size_t size) noexcept {
        return is_private_readable(address, size) && read_memory(address, destination, size);
    }

    static ImageInfo inspect_main_image() noexcept {
        ImageInfo result{};
        result.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};
        if (result.base == 0 || !read_memory(result.base, &dos, sizeof(dos)) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) {
            return result;
        }

        const auto nt_address = result.base + static_cast<uintptr_t>(dos.e_lfanew);
        if (!read_memory(nt_address, &result.nt, sizeof(result.nt)) ||
            result.nt.Signature != IMAGE_NT_SIGNATURE ||
            result.nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            result.nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return result;
        }

        result.size = result.nt.OptionalHeader.SizeOfImage;
        result.checksum = result.nt.OptionalHeader.CheckSum;
        result.section_table_rva = static_cast<uint32_t>(dos.e_lfanew +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + result.nt.FileHeader.SizeOfOptionalHeader);
        result.valid = result.size != 0 && result.base <= UINTPTR_MAX - result.size;
        return result;
    }

    static bool is_executable_range(const ImageInfo& image, uint32_t rva, size_t size) noexcept {
        if (!image.valid || size == 0 || rva >= image.size || size > image.size - rva) {
            return false;
        }

        const auto section_table = image.base + image.section_table_rva;
        const auto section_count = std::min<uint16_t>(image.nt.FileHeader.NumberOfSections, 96);
        for (uint16_t index = 0; index < section_count; ++index) {
            IMAGE_SECTION_HEADER section{};
            if (!read_memory(
                    section_table + static_cast<uintptr_t>(index) * sizeof(section),
                    &section,
                    sizeof(section))) {
                return false;
            }
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
                continue;
            }
            const auto section_size = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            const auto section_begin = section.VirtualAddress;
            const auto section_end = static_cast<uint64_t>(section_begin) + section_size;
            if (rva >= section_begin && static_cast<uint64_t>(rva) + size <= section_end) {
                return true;
            }
        }
        return false;
    }

    DiscoveryResult discover_vtable_xrefs(const ImageInfo& image) noexcept {
        constexpr size_t MAX_LOGGED_XREFS = 16;
        constexpr size_t MAX_LOGGED_DECODE_FAILURES = 12;
        constexpr size_t CONTEXT_BEFORE_INSTRUCTIONS = 8;
        constexpr size_t CONTEXT_AFTER_INSTRUCTIONS = 12;
        constexpr size_t MAX_RUNTIME_FUNCTIONS = 1'000'000;
        struct RuntimeFunctionRange {
            uint32_t begin_rva{};
            uint32_t end_rva{};
            uint32_t unwind_info_rva{};
        };
        struct XrefCandidate {
            size_t index{};
            size_t function_index{};
            uint32_t function_begin_rva{};
            uint32_t function_end_rva{};
            uint32_t xref_rva{};
            uintptr_t resolved_address{};
            uint8_t instruction_length{};
            uint8_t preceding_count{};
            bool destination_register_available{};
            unsigned destination_register_id{};
            std::array<uint32_t, CONTEXT_BEFORE_INSTRUCTIONS> preceding_rvas{};
        };
        struct DirectCreatorCaller {
            size_t index{};
            size_t function_index{};
            uint32_t function_begin_rva{};
            uint32_t function_end_rva{};
            uint32_t callsite_rva{};
            uint8_t preceding_count{};
            std::array<uint32_t, CONTEXT_BEFORE_INSTRUCTIONS> preceding_rvas{};
        };
        static_assert(sizeof(RuntimeFunctionRange) == 12);

        const auto expected_vtable = m_expected_vtable.load(std::memory_order_acquire);
        size_t xref_count{};
        size_t function_count{};
        size_t scanned_function_count{};
        size_t failed_function_count{};
        size_t scanned_bytes{};
        size_t decode_failure_details_logged{};
        std::array<XrefCandidate, MAX_LOGGED_XREFS> candidates{};
        size_t candidate_count{};
        constexpr size_t MAX_LOGGED_CREATOR_CALLERS = 16;
        std::array<DirectCreatorCaller, MAX_LOGGED_CREATOR_CALLERS> creator_callers{};
        size_t creator_caller_count{};
        size_t creator_call_count{};
        bool creator_callers_truncated{};
        bool truncated{};
        bool decode_failure_details_suppressed{};
        bool scan_complete = expected_vtable != 0 &&
            image.nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION;
        const auto exception_directory = scan_complete
            ? image.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]
            : IMAGE_DATA_DIRECTORY{};
        if (exception_directory.VirtualAddress == 0 || exception_directory.Size < sizeof(RuntimeFunctionRange) ||
            exception_directory.Size % sizeof(RuntimeFunctionRange) != 0 ||
            exception_directory.VirtualAddress >= image.size ||
            exception_directory.Size > image.size - exception_directory.VirtualAddress) {
            scan_complete = false;
        }
        if (scan_complete) {
            function_count = exception_directory.Size / sizeof(RuntimeFunctionRange);
            if (function_count > MAX_RUNTIME_FUNCTIONS) {
                scan_complete = false;
                function_count = 0;
            }
        }

        spdlog::info("[RE4XeSS][TargetStateVtableProbe] discovery-begin attempted=true imageSize=0x{:x} checksum=0x{:x} vtableRva=0x{:x} expectedVtable=0x{:x} exceptionRva=0x{:x} exceptionSize=0x{:x} functionCount={}",
            image.size, image.checksum, TARGET_STATE_VTABLE_RVA, expected_vtable,
            exception_directory.VirtualAddress, exception_directory.Size, function_count);

        const auto table_address = image.base + exception_directory.VirtualAddress;
        for (size_t function_index = 0; function_index < function_count; ++function_index) {
            RuntimeFunctionRange function{};
            if (!read_memory(table_address + function_index * sizeof(function), &function, sizeof(function))) {
                spdlog::warn("[RE4XeSS][TargetStateVtableProbe] runtime-function entry unreadable index={}", function_index);
                ++failed_function_count;
                scan_complete = false;
                break;
            }

            if (function.begin_rva >= function.end_rva || function.end_rva > image.size ||
                !is_executable_range(image, function.begin_rva, function.end_rva - function.begin_rva)) {
                ++failed_function_count;
                scan_complete = false;
                continue;
            }

            const auto function_size = static_cast<size_t>(function.end_rva - function.begin_rva);
            const auto* code = reinterpret_cast<const uint8_t*>(image.base + function.begin_rva);
            size_t offset{};
            std::array<uint32_t, CONTEXT_BEFORE_INSTRUCTIONS> previous_instruction_rvas{};
            size_t previous_instruction_count{};
            size_t previous_instruction_next{};
            bool function_decoded = true;
            while (offset < function_size) {
                const auto remaining = std::min<size_t>(15, function_size - offset);
                auto decoded = utility::decode_one(const_cast<uint8_t*>(code + offset), remaining);
                if (!decoded || decoded->Length == 0 || decoded->Length > remaining) {
                    // Keep instruction-boundary guarantees within each unwind-proven function.
                    // A bad function does not prevent independent ranges from being inspected.
                    if (decode_failure_details_logged < MAX_LOGGED_DECODE_FAILURES) {
                        spdlog::warn("[RE4XeSS][TargetStateVtableProbe] decode stopped functionIndex={} functionRva=0x{:x} functionEndRva=0x{:x} offset=0x{:x}",
                            function_index, function.begin_rva, function.end_rva, offset);
                        ++decode_failure_details_logged;
                    } else {
                        decode_failure_details_suppressed = true;
                    }
                    function_decoded = false;
                    break;
                }

                for (uint8_t operand_index = 0; operand_index < decoded->OperandsCount; ++operand_index) {
                    const auto& operand = decoded->Operands[operand_index];
                    if (operand.Type != ND_OP_MEM || !operand.Info.Memory.IsRipRel || !operand.Info.Memory.HasDisp) {
                        continue;
                    }

                    const auto instruction_address = reinterpret_cast<uintptr_t>(code + offset);
                    const auto next_instruction = instruction_address + decoded->Length;
                    const auto displacement = static_cast<int64_t>(operand.Info.Memory.Disp);
                    if ((displacement < 0 && next_instruction < static_cast<uintptr_t>(-displacement)) ||
                        (displacement >= 0 && next_instruction > UINTPTR_MAX - static_cast<uintptr_t>(displacement))) {
                        continue;
                    }
                    const auto resolved = displacement < 0
                        ? next_instruction - static_cast<uintptr_t>(-displacement)
                        : next_instruction + static_cast<uintptr_t>(displacement);
                    if (resolved != expected_vtable) {
                        continue;
                    }

                    if (xref_count < MAX_LOGGED_XREFS) {
                        auto& candidate = candidates[candidate_count++];
                        candidate.index = xref_count;
                        candidate.function_index = function_index;
                        candidate.function_begin_rva = function.begin_rva;
                        candidate.function_end_rva = function.end_rva;
                        candidate.xref_rva = function.begin_rva + static_cast<uint32_t>(offset);
                        candidate.resolved_address = resolved;
                        candidate.instruction_length = decoded->Length;
                        candidate.preceding_count = static_cast<uint8_t>(previous_instruction_count);
                        candidate.destination_register_available = decoded->OperandsCount != 0 &&
                            decoded->Operands[0].Type == ND_OP_REG;
                        candidate.destination_register_id = candidate.destination_register_available
                            ? static_cast<unsigned>(decoded->Operands[0].Info.Register.Reg)
                            : 0U;
                        const auto oldest = (previous_instruction_next + CONTEXT_BEFORE_INSTRUCTIONS -
                            previous_instruction_count) % CONTEXT_BEFORE_INSTRUCTIONS;
                        for (size_t previous_index = 0; previous_index < previous_instruction_count; ++previous_index) {
                            candidate.preceding_rvas[previous_index] = previous_instruction_rvas[
                                (oldest + previous_index) % CONTEXT_BEFORE_INSTRUCTIONS];
                        }
                    } else {
                        truncated = true;
                    }
                    ++xref_count;
                }

                if (decoded->Length == 5 && code[offset] == 0xE8) {
                    int32_t displacement{};
                    std::memcpy(&displacement, code + offset + 1, sizeof(displacement));
                    const auto call_address = reinterpret_cast<uintptr_t>(code + offset);
                    const auto target = static_cast<int64_t>(call_address + decoded->Length) + displacement;
                    if (target == static_cast<int64_t>(image.base + RE4_TARGET_STATE_CREATOR_RVA)) {
                        if (creator_call_count < MAX_LOGGED_CREATOR_CALLERS) {
                            auto& caller = creator_callers[creator_caller_count++];
                            caller.index = creator_call_count;
                            caller.function_index = function_index;
                            caller.function_begin_rva = function.begin_rva;
                            caller.function_end_rva = function.end_rva;
                            caller.callsite_rva = function.begin_rva + static_cast<uint32_t>(offset);
                            caller.preceding_count = static_cast<uint8_t>(previous_instruction_count);
                            const auto oldest = (previous_instruction_next + CONTEXT_BEFORE_INSTRUCTIONS -
                                previous_instruction_count) % CONTEXT_BEFORE_INSTRUCTIONS;
                            for (size_t previous_index = 0; previous_index < previous_instruction_count; ++previous_index) {
                                caller.preceding_rvas[previous_index] = previous_instruction_rvas[
                                    (oldest + previous_index) % CONTEXT_BEFORE_INSTRUCTIONS];
                            }
                        } else {
                            creator_callers_truncated = true;
                        }
                        ++creator_call_count;
                    }
                }

                previous_instruction_rvas[previous_instruction_next] =
                    function.begin_rva + static_cast<uint32_t>(offset);
                previous_instruction_next = (previous_instruction_next + 1) % CONTEXT_BEFORE_INSTRUCTIONS;
                previous_instruction_count = std::min(previous_instruction_count + 1, CONTEXT_BEFORE_INSTRUCTIONS);
                offset += decoded->Length;
            }
            scanned_bytes += offset;
            if (function_decoded) {
                ++scanned_function_count;
            } else {
                ++failed_function_count;
                scan_complete = false;
            }
        }

        m_vtable_discovery_complete.store(scan_complete, std::memory_order_release);
        m_vtable_discovery_done.store(true, std::memory_order_release);
        if (decode_failure_details_suppressed) {
            spdlog::warn("[RE4XeSS][TargetStateVtableProbe] further decode failure details suppressed after {} entries",
                MAX_LOGGED_DECODE_FAILURES);
        }
        spdlog::info("[RE4XeSS][TargetStateVtableProbe] discovery-summary attempted=true coverage=exception-directory-runtime-functions vtableRva=0x{:x} xrefCount={} truncated={} functions={} scannedFunctions={} failedFunctions={} decodeFailureDetailsLogged={} decodeFailureDetailsSuppressed={} scannedBytes=0x{:x} complete={}",
            TARGET_STATE_VTABLE_RVA, xref_count, truncated, function_count,
            scanned_function_count, failed_function_count, decode_failure_details_logged,
            decode_failure_details_suppressed, scanned_bytes, scan_complete);

        const auto log_context_instruction = [&](const char* kind, size_t candidate_index, const char* stage,
                                                 uint32_t rva, uint32_t function_end_rva) {
            if (rva >= function_end_rva) {
                return false;
            }
            const auto remaining = std::min<size_t>(15, function_end_rva - rva);
            const auto address = image.base + rva;
            auto decoded = utility::decode_one(reinterpret_cast<uint8_t*>(address), remaining);
            if (!decoded || decoded->Length == 0 || decoded->Length > remaining) {
                spdlog::warn("[RE4XeSS][TargetStateVtableProbe] context decode failed kind={} candidate={} stage={} rva=0x{:x}",
                    kind, candidate_index, stage, rva);
                return false;
            }

            std::array<char, 128> instruction_text{};
            const auto text_status = NdToText(&*decoded, address,
                static_cast<uint32_t>(instruction_text.size()), instruction_text.data());
            std::ostringstream bytes;
            for (uint8_t byte_index = 0; byte_index < decoded->Length; ++byte_index) {
                if (byte_index != 0) bytes << ' ';
                bytes << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(reinterpret_cast<const uint8_t*>(address)[byte_index]);
            }
            spdlog::info("[RE4XeSS][TargetStateVtableProbe] context kind={} candidate={} rva=0x{:x} stage={} text={} bytes={}",
                kind, candidate_index, rva, stage,
                text_status == ND_STATUS_SUCCESS ? instruction_text.data() : "unavailable", bytes.str());
            return true;
        };

        for (size_t candidate_index = 0; candidate_index < candidate_count; ++candidate_index) {
            const auto& candidate = candidates[candidate_index];
            const auto offset = candidate.xref_rva - candidate.function_begin_rva;
            spdlog::info("[RE4XeSS][TargetStateVtableProbe] candidate index={} xrefRva=0x{:x} instructionLength={} resolved=0x{:x} functionIndex={} functionBeginRva=0x{:x} functionEndRva=0x{:x} offset=0x{:x} classification=unknown destinationRegisterAvailable={} destinationRegisterId={}",
                candidate.index, candidate.xref_rva, candidate.instruction_length, candidate.resolved_address, candidate.function_index,
                candidate.function_begin_rva, candidate.function_end_rva, offset,
                candidate.destination_register_available, candidate.destination_register_id);
            for (size_t previous_index = 0; previous_index < candidate.preceding_count; ++previous_index) {
                (void)log_context_instruction("vtable-xref", candidate.index, "before",
                    candidate.preceding_rvas[previous_index], candidate.function_end_rva);
            }
            if (!log_context_instruction("vtable-xref", candidate.index, "at", candidate.xref_rva, candidate.function_end_rva)) {
                continue;
            }

            auto after_rva = candidate.xref_rva + candidate.instruction_length;
            for (size_t after_index = 0; after_index < CONTEXT_AFTER_INSTRUCTIONS &&
                after_rva < candidate.function_end_rva; ++after_index) {
                const auto remaining = std::min<size_t>(15, candidate.function_end_rva - after_rva);
                auto decoded = utility::decode_one(reinterpret_cast<uint8_t*>(image.base + after_rva), remaining);
                if (!decoded || decoded->Length == 0 || decoded->Length > remaining) {
                    spdlog::warn("[RE4XeSS][TargetStateVtableProbe] context decode failed kind=vtable-xref candidate={} stage=after rva=0x{:x}",
                        candidate.index, after_rva);
                    break;
                }
                (void)log_context_instruction("vtable-xref", candidate.index, "after", after_rva, candidate.function_end_rva);
                after_rva += decoded->Length;
            }
        }

        for (size_t caller_index = 0; caller_index < creator_caller_count; ++caller_index) {
            const auto& caller = creator_callers[caller_index];
            const auto offset = caller.callsite_rva - caller.function_begin_rva;
            spdlog::info("[RE4XeSS][TargetStateCreatorProbe] direct-caller index={} callsiteRva=0x{:x} functionIndex={} functionBeginRva=0x{:x} functionEndRva=0x{:x} offset=0x{:x} targetRva=0x{:x}",
                caller.index, caller.callsite_rva, caller.function_index,
                caller.function_begin_rva, caller.function_end_rva, offset,
                RE4_TARGET_STATE_CREATOR_RVA);
            for (size_t previous_index = 0; previous_index < caller.preceding_count; ++previous_index) {
                (void)log_context_instruction("creator-caller", caller.index, "before",
                    caller.preceding_rvas[previous_index], caller.function_end_rva);
            }
            if (!log_context_instruction("creator-caller", caller.index, "at",
                    caller.callsite_rva, caller.function_end_rva)) {
                continue;
            }
            auto after_rva = caller.callsite_rva + 5;
            for (size_t after_index = 0; after_index < CONTEXT_AFTER_INSTRUCTIONS &&
                after_rva < caller.function_end_rva; ++after_index) {
                const auto remaining = std::min<size_t>(15, caller.function_end_rva - after_rva);
                auto decoded = utility::decode_one(reinterpret_cast<uint8_t*>(image.base + after_rva), remaining);
                if (!decoded || decoded->Length == 0 || decoded->Length > remaining) {
                    spdlog::warn("[RE4XeSS][TargetStateVtableProbe] context decode failed kind=creator-caller candidate={} stage=after rva=0x{:x}",
                        caller.index, after_rva);
                    break;
                }
                (void)log_context_instruction("creator-caller", caller.index, "after",
                    after_rva, caller.function_end_rva);
                after_rva += decoded->Length;
            }
        }
        spdlog::info("[RE4XeSS][TargetStateCreatorProbe] direct-caller-summary targetRva=0x{:x} callerCount={} logged={} truncated={} coverage=exception-directory-runtime-functions",
            RE4_TARGET_STATE_CREATOR_RVA, creator_call_count, creator_caller_count, creator_callers_truncated);

        return DiscoveryResult{ true, scan_complete, xref_count };
    }

    static bool matches_bytes(uintptr_t address, std::initializer_list<uint8_t> expected) noexcept {
        std::array<uint8_t, 32> bytes{};
        if (expected.size() > bytes.size() || !read_memory(address, bytes.data(), expected.size())) {
            return false;
        }
        return std::equal(expected.begin(), expected.end(), bytes.begin());
    }

    bool refresh_live_overlay_anchor(
        sdk::renderer::layer::Overlay* overlay,
        sdk::renderer::TargetState* overlay_state,
        uint64_t frame_id) noexcept {
        const auto overlay_address = reinterpret_cast<uintptr_t>(overlay);
        const auto slot_address = reinterpret_cast<uintptr_t>(&overlay->get_main_target_state());
        uintptr_t current_state{};
        const bool slot_valid = overlay_state != nullptr && overlay_address != 0 && slot_address >= overlay_address &&
            slot_address - overlay_address == RE4_TARGET_STATE_SLOT_OFFSET &&
            read_private_memory(slot_address, &current_state, sizeof(current_state)) &&
            current_state == reinterpret_cast<uintptr_t>(overlay_state);
        if (!slot_valid) {
            if (!m_live_anchor_invalid_logged.exchange(true, std::memory_order_acq_rel)) {
                spdlog::warn("[RE4XeSS][TargetStateVtableProbe] live-anchor unavailable overlay=0x{:x} slot=0x{:x} offset=0x{:x} expectedOffset=0x{:x} slotReadable={} slotValue=0x{:x} accessorValue=0x{:x}",
                    overlay_address, slot_address,
                    slot_address >= overlay_address ? slot_address - overlay_address : 0,
                    RE4_TARGET_STATE_SLOT_OFFSET,
                    is_private_readable(slot_address, sizeof(current_state)),
                    current_state,
                    reinterpret_cast<uintptr_t>(overlay_state));
            }
            return false;
        }

        if (!m_overlay_snapshot_attempted) {
            m_overlay_snapshot_attempted = true;
            m_overlay_state = current_state;
            m_overlay_desc = m_overlay_state + RE4_RENDER_RESOURCE_SIZE;
            m_overlay_snapshot_valid = read_private_memory(m_overlay_state, &m_overlay_snapshot, sizeof(m_overlay_snapshot));
            m_overlay_rtv_array = m_overlay_snapshot_valid ? m_overlay_snapshot.rtvs : 0;
            m_overlay_rtv = 0;
            if (m_overlay_snapshot_valid && m_overlay_snapshot.num_rtv != 0 &&
                m_overlay_snapshot.num_rtv <= MAX_TARGET_STATE_RTVS && m_overlay_snapshot.rtvs != 0) {
                // Read the intrusive pointer's raw value only; do not AddRef/Release it.
                read_private_memory(m_overlay_snapshot.rtvs, &m_overlay_rtv, sizeof(m_overlay_rtv));
            }
        }
        if (!m_live_anchor_logged.exchange(true, std::memory_order_acq_rel)) {
            const auto expected_vtable = m_expected_vtable.load(std::memory_order_acquire);
            const bool snapshot_readable = m_overlay_snapshot_valid;
            const bool discovery_complete = m_vtable_discovery_complete.load(std::memory_order_acquire);
            const bool image_identity_valid = m_image_identity_valid.load(std::memory_order_acquire);
            const bool match = image_identity_valid && snapshot_readable && expected_vtable != 0 &&
                m_overlay_snapshot.vtable == expected_vtable;
            m_live_anchor_trusted.store(match, std::memory_order_release);
            spdlog::info("[RE4XeSS][TargetStateVtableProbe] live-anchor state=0x{:x} liveVtable=0x{:x} expectedVtable=0x{:x} imageIdentityValid={} match={} discoveryComplete={} trusted={} frame={}",
                current_state, snapshot_readable ? m_overlay_snapshot.vtable : 0, expected_vtable,
                image_identity_valid, match, discovery_complete, match,
                static_cast<unsigned long long>(frame_id));
        }
        return true;
    }

    static bool relative_call_targets(uintptr_t call_address, uint32_t expected_target_rva) noexcept {
        std::array<uint8_t, 5> bytes{};
        if (!read_memory(call_address, bytes.data(), bytes.size()) || bytes[0] != 0xE8) {
            return false;
        }
        int32_t displacement{};
        std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
        const auto target = static_cast<int64_t>(call_address + bytes.size()) + displacement;
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return base != 0 && target == static_cast<int64_t>(base + expected_target_rva);
    }

    static bool validate_creator_function_range(const ImageInfo& image) noexcept {
        struct RuntimeFunctionRange {
            uint32_t begin_rva{};
            uint32_t end_rva{};
            uint32_t unwind_info_rva{};
        };
        static_assert(sizeof(RuntimeFunctionRange) == 12);
        if (image.nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
            return false;
        }
        const auto directory = image.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(RuntimeFunctionRange) ||
            directory.Size % sizeof(RuntimeFunctionRange) != 0 || directory.VirtualAddress >= image.size ||
            directory.Size > image.size - directory.VirtualAddress) {
            return false;
        }

        const auto count = directory.Size / sizeof(RuntimeFunctionRange);
        const auto table = image.base + directory.VirtualAddress;
        for (uint32_t index = 0; index < count; ++index) {
            RuntimeFunctionRange range{};
            if (!read_memory(table + static_cast<uintptr_t>(index) * sizeof(range), &range, sizeof(range))) {
                return false;
            }
            if (range.begin_rva == RE4_TARGET_STATE_CREATOR_RVA &&
                range.end_rva == RE4_TARGET_STATE_CREATOR_END_RVA) {
                return true;
            }
        }
        return false;
    }

    bool arm_creator_probe_early(const ImageInfo& image) noexcept {
        if (m_creator_attempted.exchange(true, std::memory_order_acq_rel)) {
            return m_creator_active.load(std::memory_order_acquire);
        }

        const bool bytes_valid = image.valid && image.size == EXPECTED_IMAGE_SIZE &&
            image.checksum == EXPECTED_IMAGE_CHECKSUM &&
            validate_creator_function_range(image) &&
            is_executable_range(image, RE4_TARGET_STATE_CREATOR_RVA,
                RE4_TARGET_STATE_CREATOR_END_RVA - RE4_TARGET_STATE_CREATOR_RVA) &&
            matches_bytes(image.base + 0x47D2180, {
                0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
                0x48, 0x8B, 0xFA, 0xB9, 0x01, 0x00, 0x00, 0x00 }) &&
            matches_bytes(image.base + 0x47D2192, { 0xBA, 0xA8, 0x00, 0x00, 0x00 }) &&
            relative_call_targets(image.base + 0x47D2197, 0x3AB27F0) &&
            matches_bytes(image.base + 0x47D219C, { 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0 }) &&
            relative_call_targets(image.base + 0x47D21AA, 0x446E790) &&
            matches_bytes(image.base + 0x47D21AF, { 0x48, 0x8D, 0x05, 0x92, 0x9F, 0x34, 0x03 }) &&
            matches_bytes(image.base + 0x47D21B6, { 0x48, 0x89, 0x03 }) &&
            matches_bytes(image.base + 0x47D21B9, { 0x48, 0x8B, 0xC3 }) &&
            matches_bytes(image.base + 0x47D21BC, { 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3 }) &&
            matches_bytes(image.base + 0x47D21C7, { 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3 });
        if (!bytes_valid) {
            spdlog::warn("[RE4XeSS][TargetStateCreatorProbe] not armed: RE4 image/function/byte validation failed imageSize=0x{:x} checksum=0x{:x} functionBeginRva=0x{:x} functionEndRva=0x{:x}",
                image.size, image.checksum, RE4_TARGET_STATE_CREATOR_RVA, RE4_TARGET_STATE_CREATOR_END_RVA);
            return false;
        }

        constexpr std::array<uint32_t, static_cast<size_t>(CreatorHookPoint::Count)> hook_rvas{
            0x47D2180, 0x47D219C, 0x47D21AF, 0x47D21BC, 0x47D21C7 };
        for (size_t index = 0; index < hook_rvas.size(); ++index) {
            m_creator_hooks[index] = safetyhook::create_mid(
                reinterpret_cast<void*>(image.base + hook_rvas[index]),
                creator_hook_callback(index), safetyhook::MidHook::StartDisabled);
            if (!m_creator_hooks[index]) {
                disarm_creator_probe("could not create all creator hooks");
                return false;
            }
            const auto actual_span = m_creator_hooks[index].original_bytes().size();
            const auto next_rva = index + 1 < hook_rvas.size()
                ? hook_rvas[index + 1]
                : RE4_TARGET_STATE_CREATOR_END_RVA;
            if (actual_span == 0 || actual_span > next_rva - hook_rvas[index]) {
                spdlog::warn("[RE4XeSS][TargetStateCreatorProbe] not armed: hook span overlaps next site index={} rva=0x{:x} actualSpan={} maxSpan={}",
                    index, hook_rvas[index], actual_span, next_rva - hook_rvas[index]);
                disarm_creator_probe("creator hook span overlap");
                return false;
            }
            spdlog::info("[RE4XeSS][TargetStateCreatorProbe] hook-span validated point={} rva=0x{:x} actual={} nextRva=0x{:x}",
                index, hook_rvas[index], actual_span, next_rva);
        }

        m_creator_call_count.store(0, std::memory_order_release);
        m_creator_ready_count.store(0, std::memory_order_release);
        m_creator_frames.store(0, std::memory_order_release);
        m_creator_disarm_started.store(false, std::memory_order_release);
        for (size_t index = 0; index < m_creator_hooks.size(); ++index) {
            if (!m_creator_hooks[index].enable()) {
                disarm_creator_probe("could not enable all creator hooks");
                return false;
            }
        }
        m_creator_active.store(true, std::memory_order_release);

        spdlog::info("[RE4XeSS][TargetStateCreatorProbe] armed creatorRva=0x{:x} functionBeginRva=0x{:x} functionEndRva=0x{:x} imageSize=0x{:x} checksum=0x{:x} points=entry,post-allocation,post-initializer,post-vtable-store-return,allocation-failure-return observationLimit={} frameLimit={} readOnly=true",
            RE4_TARGET_STATE_CREATOR_RVA, RE4_TARGET_STATE_CREATOR_RVA, RE4_TARGET_STATE_CREATOR_END_RVA,
            image.size, image.checksum, MAX_CREATOR_OBSERVATIONS, MAX_CREATOR_CAPTURE_FRAMES);
        return true;
    }

    static safetyhook::MidHookFn creator_hook_callback(size_t index) noexcept {
        switch (static_cast<CreatorHookPoint>(index)) {
        case CreatorHookPoint::FunctionEntry: return &creator_entry_callback;
        case CreatorHookPoint::AllocationReturned: return &creator_allocation_callback;
        case CreatorHookPoint::InitializerReturned: return &creator_initializer_callback;
        case CreatorHookPoint::SuccessReturn: return &creator_success_return_callback;
        case CreatorHookPoint::AllocationFailureReturn: return &creator_failure_return_callback;
        default: return nullptr;
        }
    }

    static void creator_entry_callback(safetyhook::Context& context) noexcept {
        instance().capture_creator_entry(context);
    }

    static void creator_allocation_callback(safetyhook::Context& context) noexcept {
        instance().capture_creator_phase(CreatorHookPoint::AllocationReturned, context);
    }

    static void creator_initializer_callback(safetyhook::Context& context) noexcept {
        instance().capture_creator_phase(CreatorHookPoint::InitializerReturned, context);
    }

    static void creator_success_return_callback(safetyhook::Context& context) noexcept {
        instance().capture_creator_phase(CreatorHookPoint::SuccessReturn, context);
    }

    static void creator_failure_return_callback(safetyhook::Context& context) noexcept {
        instance().capture_creator_phase(CreatorHookPoint::AllocationFailureReturn, context);
    }

    void capture_creator_entry(const safetyhook::Context& context) noexcept {
        if (!m_creator_active.load(std::memory_order_acquire)) {
            return;
        }
        if (s_creator_active_depth >= MAX_CREATOR_ACTIVE_DEPTH) {
            return;
        }

        auto index = m_creator_call_count.load(std::memory_order_acquire);
        while (index < MAX_CREATOR_OBSERVATIONS &&
            !m_creator_call_count.compare_exchange_weak(index, index + 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        if (index >= MAX_CREATOR_OBSERVATIONS) {
            return;
        }

        auto& observation = m_creator_observations[index];
        uintptr_t caller_return{};
        (void)read_memory(static_cast<uintptr_t>(context.rsp), &caller_return, sizeof(caller_return));
        uintptr_t caller_callsite{};
        if (caller_return >= m_image_base + 5 &&
            relative_call_targets(caller_return - 5, RE4_TARGET_STATE_CREATOR_RVA)) {
            caller_callsite = caller_return - 5 - m_image_base;
        }
        observation.id = index + 1;
        observation.thread_id = GetCurrentThreadId();
        observation.caller_return_rva = caller_return >= m_image_base && caller_return < m_image_end
            ? caller_return - m_image_base
            : 0;
        observation.caller_callsite_rva = caller_callsite;
        observation.rcx = static_cast<uintptr_t>(context.rcx);
        observation.rdx = static_cast<uintptr_t>(context.rdx);
        observation.r8 = static_cast<uintptr_t>(context.r8);
        observation.r9 = static_cast<uintptr_t>(context.r9);
        observation.rdi = static_cast<uintptr_t>(context.rdi);
        observation.ready.store(true, std::memory_order_release);
        m_creator_ready_count.fetch_add(1, std::memory_order_acq_rel);
        s_creator_active_calls[s_creator_active_depth++] = index;
        spdlog::info("[RE4XeSS][TargetStateCreatorProbe] stage=entry id={} tid={} callerReturnRva=0x{:x} callerCallsiteRva=0x{:x} regs[rcx=0x{:x},rdx=0x{:x},r8=0x{:x},r9=0x{:x},rdi=0x{:x},rsp=0x{:x}]",
            observation.id, observation.thread_id, observation.caller_return_rva,
            observation.caller_callsite_rva, observation.rcx, observation.rdx,
            observation.r8, observation.r9, observation.rdi, static_cast<uintptr_t>(context.rsp));
    }

    void capture_creator_phase(CreatorHookPoint point, const safetyhook::Context& context) noexcept {
        if (!m_creator_active.load(std::memory_order_acquire) || s_creator_active_depth == 0) {
            return;
        }
        const auto index = s_creator_active_calls[s_creator_active_depth - 1];
        if (index >= MAX_CREATOR_OBSERVATIONS) {
            return;
        }
        auto& observation = m_creator_observations[index];
        const auto thread_id = GetCurrentThreadId();
        if (!observation.ready.load(std::memory_order_acquire) || observation.thread_id != thread_id) {
            return;
        }

        if (point == CreatorHookPoint::AllocationReturned) {
            const auto allocation = static_cast<uintptr_t>(context.rax);
            observation.allocation_result.store(allocation, std::memory_order_release);
            spdlog::info("[RE4XeSS][TargetStateCreatorProbe] stage=post-allocation id={} tid={} allocationResult=0x{:x} expectedSize=0xa8 rbXBeforeMove=0x{:x}",
                observation.id, thread_id, allocation, static_cast<uintptr_t>(context.rbx));
            return;
        }
        if (point == CreatorHookPoint::InitializerReturned) {
            const auto object = static_cast<uintptr_t>(context.rbx);
            observation.initialized_object.store(object, std::memory_order_release);
            spdlog::info("[RE4XeSS][TargetStateCreatorProbe] stage=post-initializer id={} tid={} object=0x{:x} rax=0x{:x} rdi=0x{:x} rcx=0x{:x} rdx=0x{:x}",
                observation.id, thread_id, object, static_cast<uintptr_t>(context.rax),
                static_cast<uintptr_t>(context.rdi), static_cast<uintptr_t>(context.rcx),
                static_cast<uintptr_t>(context.rdx));
            return;
        }

        if (point == CreatorHookPoint::SuccessReturn) {
            const auto object = static_cast<uintptr_t>(context.rbx);
            const auto returned = static_cast<uintptr_t>(context.rax);
            observation.constructed_object.store(object, std::memory_order_release);
            observation.returned_object.store(returned, std::memory_order_release);
            const auto snapshot = snapshot_creator_object(object);
            observation.object_vtable.store(snapshot.vtable, std::memory_order_release);
            observation.object_ref_count.store(snapshot.ref_count, std::memory_order_release);
            observation.object_render_frame.store(snapshot.render_frame, std::memory_order_release);
            observation.object_rtvs.store(snapshot.rtvs, std::memory_order_release);
            observation.object_dsv.store(snapshot.dsv, std::memory_order_release);
            observation.object_num_rtv.store(snapshot.num_rtv, std::memory_order_release);
            observation.object_rect_left.store(snapshot.rect_left, std::memory_order_release);
            observation.object_rect_top.store(snapshot.rect_top, std::memory_order_release);
            observation.object_rect_right.store(snapshot.rect_right, std::memory_order_release);
            observation.object_rect_bottom.store(snapshot.rect_bottom, std::memory_order_release);
            observation.object_flag.store(snapshot.flag, std::memory_order_release);
            observation.object_rtv0.store(snapshot.rtv0, std::memory_order_release);
            observation.object_rtv_valid.store(snapshot.rtv_valid, std::memory_order_release);
            observation.object_rtv_format.store(snapshot.rtv_format, std::memory_order_release);
            observation.object_rtv_dimension.store(snapshot.rtv_dimension, std::memory_order_release);
            observation.object_texture.store(snapshot.texture, std::memory_order_release);
            observation.object_texture_header_valid.store(snapshot.texture_header_valid, std::memory_order_release);
            observation.object_texture_vtable.store(snapshot.texture_vtable, std::memory_order_release);
            observation.object_texture_ref_count.store(snapshot.texture_ref_count, std::memory_order_release);
            observation.object_texture_render_frame.store(snapshot.texture_render_frame, std::memory_order_release);
            observation.object_target_state_like.store(snapshot.target_state_like, std::memory_order_release);
            spdlog::info("[RE4XeSS][TargetStateCreatorProbe] stage=post-vtable-store-return id={} tid={} candidateObject=0x{:x} returnedObject=0x{:x} objectSnapshotReadable={} vtable=0x{:x} refCount={} renderFrame={} rtvs=0x{:x} dsv=0x{:x} numRtv={} rect=({:.3f},{:.3f},{:.3f},{:.3f}) flag={} rtv0=0x{:x} rtvValid={} rtvFormat={} rtvDimension={} texture=0x{:x} textureHeaderValid={} textureVtable=0x{:x} textureRefCount={} textureRenderFrame={} targetStateLike={}",
                observation.id, thread_id, object, returned, snapshot.readable, snapshot.vtable,
                snapshot.ref_count, snapshot.render_frame, snapshot.rtvs, snapshot.dsv,
                snapshot.num_rtv, snapshot.rect_left, snapshot.rect_top,
                snapshot.rect_right, snapshot.rect_bottom, snapshot.flag, snapshot.rtv0,
                snapshot.rtv_valid, snapshot.rtv_format, snapshot.rtv_dimension,
                snapshot.texture, snapshot.texture_header_valid, snapshot.texture_vtable,
                snapshot.texture_ref_count, snapshot.texture_render_frame, snapshot.target_state_like);
        } else if (point == CreatorHookPoint::AllocationFailureReturn) {
            const auto returned = static_cast<uintptr_t>(context.rax);
            observation.returned_object.store(returned, std::memory_order_release);
            spdlog::info("[RE4XeSS][TargetStateCreatorProbe] stage=allocation-failure-return id={} tid={} allocationResult=0x{:x} returnedObject=0x{:x}",
                observation.id, thread_id,
                observation.allocation_result.load(std::memory_order_acquire), returned);
        }

        if (s_creator_active_calls[s_creator_active_depth - 1] == index) {
            --s_creator_active_depth;
        }
    }

    void disarm_creator_probe(std::string_view reason) noexcept {
        m_creator_active.store(false, std::memory_order_release);
        if (m_creator_disarm_started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        bool disable_failed{};
        for (auto& hook : m_creator_hooks) {
            if (hook && !hook.disable()) {
                disable_failed = true;
            }
        }
        spdlog::info("[RE4XeSS][TargetStateCreatorProbe] disarmed reason={} observations={} frames={} disableFailed={}",
            reason, m_creator_ready_count.load(std::memory_order_acquire),
            m_creator_frames.load(std::memory_order_acquire), disable_failed);
    }

    void correlate_creator_observations(uintptr_t live_overlay_state) noexcept {
        if (live_overlay_state == 0) {
            return;
        }
        const auto ready_count = m_creator_ready_count.load(std::memory_order_acquire);
        if (live_overlay_state == m_last_creator_correlation_state &&
            ready_count == m_last_creator_correlation_count) {
            return;
        }
        m_last_creator_correlation_state = live_overlay_state;
        m_last_creator_correlation_count = ready_count;
        const auto live = snapshot_creator_object(live_overlay_state);
        bool matched{};
        uint64_t matched_call_id{};
        uintptr_t matched_caller_rva{};
        uint32_t structural_matches{};
        uint64_t first_structural_call_id{};
        uintptr_t first_structural_caller_rva{};
        for (size_t index = 0; index < MAX_CREATOR_OBSERVATIONS; ++index) {
            const auto& observation = m_creator_observations[index];
            if (!observation.ready.load(std::memory_order_acquire)) {
                continue;
            }
            const auto constructed = observation.constructed_object.load(std::memory_order_acquire);
            const auto returned = observation.returned_object.load(std::memory_order_acquire);
            if (constructed == live_overlay_state || returned == live_overlay_state) {
                matched = true;
                matched_call_id = observation.id;
                matched_caller_rva = observation.caller_callsite_rva != 0
                    ? observation.caller_callsite_rva
                    : observation.caller_return_rva;
                break;
            }

            if (constructed == 0 ||
                !observation.object_target_state_like.load(std::memory_order_acquire)) {
                continue;
            }
            const bool vtable_match = live.readable &&
                observation.object_vtable.load(std::memory_order_acquire) == live.vtable && live.vtable != 0;
            const bool count_match = observation.object_num_rtv.load(std::memory_order_acquire) == live.num_rtv;
            const auto candidate_rtv = observation.object_rtv0.load(std::memory_order_acquire);
            const bool rtv_match = candidate_rtv == 0 || live.rtv0 == 0 || candidate_rtv == live.rtv0;
            const auto rect_left = observation.object_rect_left.load(std::memory_order_acquire);
            const auto rect_top = observation.object_rect_top.load(std::memory_order_acquire);
            const auto rect_right = observation.object_rect_right.load(std::memory_order_acquire);
            const auto rect_bottom = observation.object_rect_bottom.load(std::memory_order_acquire);
            const bool rect_match = live.readable && std::abs(rect_left - live.rect_left) <= 0.01f &&
                std::abs(rect_top - live.rect_top) <= 0.01f &&
                std::abs(rect_right - live.rect_right) <= 0.01f &&
                std::abs(rect_bottom - live.rect_bottom) <= 0.01f;
            if (vtable_match && count_match && rtv_match && rect_match) {
                if (structural_matches == 0) {
                    first_structural_call_id = observation.id;
                    first_structural_caller_rva = observation.caller_callsite_rva != 0
                        ? observation.caller_callsite_rva
                        : observation.caller_return_rva;
                }
                ++structural_matches;
            }
        }

        if (!matched && structural_matches != 0) {
            matched_call_id = first_structural_call_id;
            matched_caller_rva = first_structural_caller_rva;
        }
        spdlog::info("[RE4XeSS][TargetStateCreatorProbe] live-correlation liveOverlayState=0x{:x} matchedObservation={} matchedCallId={} creatorRva=0x{:x} callerRva=0x{:x} structuralMatches={} liveSnapshotReadable={} liveVtable=0x{:x} liveRefCount={} liveNumRtv={} liveRtv0=0x{:x} liveRect=({:.3f},{:.3f},{:.3f},{:.3f}) observations={}",
            live_overlay_state, matched, matched_call_id, RE4_TARGET_STATE_CREATOR_RVA,
            matched_caller_rva, structural_matches, live.readable, live.vtable,
            live.ref_count, live.num_rtv, live.rtv0, live.rect_left, live.rect_top,
            live.rect_right, live.rect_bottom, ready_count);
    }

    static bool validate_provenance_writer(const ImageInfo& image) noexcept {
        return image.valid && image.size == EXPECTED_IMAGE_SIZE && image.checksum == EXPECTED_IMAGE_CHECKSUM &&
            is_executable_range(image, 0x4597A0, 0x9A) &&
            matches_bytes(image.base + 0x4597A0, {
                0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 }) &&
            matches_bytes(image.base + 0x4597B7, {
                0x48, 0x8B, 0x05, 0x32, 0x87, 0x23, 0x0D, 0x48, 0x8B, 0x78, 0x60 }) &&
            matches_bytes(image.base + 0x4597C9, { 0x48, 0x89, 0xBA, 0x90, 0x00, 0x00, 0x00 }) &&
            matches_bytes(image.base + 0x4597FB, { 0xF0, 0x48, 0x0F, 0xB1, 0xBB, 0x90, 0x00, 0x00, 0x00 }) &&
            matches_bytes(image.base + 0x45981A, { 0xF0, 0x48, 0x0F, 0xB1, 0xBB, 0x90, 0x00, 0x00, 0x00 }) &&
            matches_bytes(image.base + 0x4597DA, { 0xC3 }) &&
            matches_bytes(image.base + 0x459839, { 0xC3 }) &&
            relative_call_targets(image.base + 0x4597EC, 0x39D6ED0) &&
            relative_call_targets(image.base + 0x45982A, 0x39DF010);
    }

    static bool validate_callsite(const ImageInfo& image, size_t index) noexcept {
        if (index >= SITE_COUNT) {
            return false;
        }

        switch (index) {
        case 0: {
            if (!is_executable_range(image, 0x447AF50, 11) ||
                !is_executable_range(image, PRE_HOOK_RVAS[index], 7) ||
                !is_executable_range(image, CALLSITE_RVAS[index], 5) ||
                !is_executable_range(image, RETURN_RVAS[index], 4) ||
                !matches_bytes(image.base + 0x447AF50, {
                    0x49, 0x8B, 0xCC, 0x44, 0x89, 0xAD, 0xD0, 0x06, 0x00, 0x00, 0xE8 })) {
                return false;
            }
            int32_t displacement{};
            if (!read_memory(image.base + CALLSITE_RVAS[index] + 1, &displacement, sizeof(displacement))) {
                return false;
            }
            const auto target = static_cast<int64_t>(CALLSITE_RVAS[index]) + 5 + displacement;
            return target == 0x4470470 && is_executable_range(image, static_cast<uint32_t>(target), 1) &&
                matches_bytes(image.base + RETURN_RVAS[index], { 0x0F, 0xB7, 0x4B, 0x10 });
        }
        case 1:
            return is_executable_range(image, 0x47212A0, 9) &&
                is_executable_range(image, PRE_HOOK_RVAS[index], 6) &&
                is_executable_range(image, RETURN_RVAS[index], 4) &&
                matches_bytes(image.base + 0x47212A0, {
                       0x48, 0x8B, 0x07, 0x48, 0x8B, 0xCF, 0xFF, 0x50, 0x40 }) &&
                matches_bytes(image.base + RETURN_RVAS[index], { 0x48, 0x8B, 0x5F, 0x48 });
        case 2:
            return is_executable_range(image, 0x44C7A21, 9) &&
                is_executable_range(image, PRE_HOOK_RVAS[index], 6) &&
                is_executable_range(image, RETURN_RVAS[index], 12) &&
                matches_bytes(image.base + 0x44C7A21, {
                       0x49, 0x8B, 0x06, 0x49, 0x8B, 0xCE, 0xFF, 0x50, 0x40 }) &&
                matches_bytes(image.base + RETURN_RVAS[index], {
                    0x48, 0xC7, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
        case 3:
            return is_executable_range(image, 0x47D0FBE, 23) &&
                is_executable_range(image, PRE_HOOK_RVAS[index], 7) &&
                is_executable_range(image, CALLSITE_RVAS[index], 6) &&
                is_executable_range(image, RETURN_RVAS[index], 5) &&
                matches_bytes(image.base + PRE_HOOK_RVAS[index], {
                    0x48, 0x8B, 0x52, 0x18, 0x48, 0x8B, 0x01 }) &&
                matches_bytes(image.base + 0x47D0FBE, {
                       0x48, 0x8B, 0x88, 0x08, 0x6C, 0xC0, 0x00, 0x4D, 0x8B, 0xCF,
                       0x48, 0x8B, 0x52, 0x18, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0xA0, 0x00, 0x00, 0x00 }) &&
                matches_bytes(image.base + RETURN_RVAS[index], { 0xBA, 0xC0, 0x00, 0x00, 0x00 });
        default:
            return false;
        }
    }

    static uint32_t hook_rva(size_t hook_index) noexcept {
        if (hook_index < SITE_COUNT) {
            return PRE_HOOK_RVAS[hook_index];
        }
        const auto site = hook_index - SITE_COUNT;
        return site < RETURN_RVAS.size() ? RETURN_RVAS[site] : 0;
    }

    template <size_t Site>
    static void before_call(safetyhook::Context& context) {
        instance().capture_before(Site, context);
    }

    template <size_t Site>
    static void after_call(safetyhook::Context& context) {
        instance().capture_after(Site, context);
    }

    template <size_t Site>
    static void capture_provenance_write_callback(safetyhook::Context& context) {
        instance().capture_provenance_write(Site, context);
    }

    static safetyhook::MidHookFn hook_callback(size_t hook_index) noexcept {
        static constexpr std::array<safetyhook::MidHookFn, HOOK_COUNT> callbacks{
            &before_call<0>,
            &before_call<1>,
            &before_call<2>,
            &before_call<3>,
            &after_call<0>,
            &after_call<1>,
            &after_call<2>,
            &after_call<3>,
        };
        return hook_index < callbacks.size() ? callbacks[hook_index] : nullptr;
    }

    static safetyhook::MidHookFn provenance_hook_callback(size_t hook_index) noexcept {
        static constexpr std::array<safetyhook::MidHookFn, PROVENANCE_HOOK_COUNT> callbacks{
            &capture_provenance_write_callback<0>,
            &capture_provenance_write_callback<1>,
            &capture_provenance_write_callback<2>,
        };
        return hook_index < callbacks.size() ? callbacks[hook_index] : nullptr;
    }

    bool arm_provenance_hooks(const ImageInfo& image) noexcept {
        (void)image;
        spdlog::warn("[RE4XeSS][TargetStateVtableProbe] retired writer hook installation refused rva=0x{:x}",
            RETIRED_RE4_TARGET_STATE_WRITER_RVA);
        return false;
    }

    void disarm_provenance_hooks(std::string_view reason) noexcept {
        m_provenance_active.store(false, std::memory_order_release);
        if (!m_provenance_attempted.load(std::memory_order_acquire) ||
            m_provenance_disarm_started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        bool disable_failed{};
        for (auto& hook : m_provenance_hooks) {
            if (hook && !hook.disable()) {
                disable_failed = true;
            }
        }
        report_early_provenance_observations();
        spdlog::info("[RE4XeSS][TargetStateProvenance] disarmed reason={} frames={} writes={} confirmedAssignments={} earlyCaptured={} earlyCorrelated={} earlyOverflow={} overlayKnown={} pendingState={} disableFailed={}",
            reason,
            m_provenance_frames.load(std::memory_order_acquire),
            m_provenance_write_count.load(std::memory_order_acquire),
            m_provenance_confirmed_count.load(std::memory_order_acquire),
            std::min<uint32_t>(m_early_provenance_count.load(std::memory_order_acquire),
                static_cast<uint32_t>(MAX_EARLY_PROVENANCE_WRITES)),
            m_early_provenance_correlated_count.load(std::memory_order_acquire),
            m_early_provenance_overflow.load(std::memory_order_acquire),
            m_provenance_overlay_slot.load(std::memory_order_acquire) != 0,
            m_provenance_pending_state.load(std::memory_order_acquire),
            disable_failed);
    }

    void capture_unbound_provenance_write(size_t hook_index, uintptr_t receiver, const safetyhook::Context& context) noexcept {
        auto index = m_early_provenance_count.load(std::memory_order_acquire);
        while (index < MAX_EARLY_PROVENANCE_WRITES &&
            !m_early_provenance_count.compare_exchange_weak(
                index, index + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        if (index >= MAX_EARLY_PROVENANCE_WRITES) {
            m_early_provenance_overflow.store(true, std::memory_order_release);
            return;
        }

        static constexpr std::array<uint32_t, PROVENANCE_HOOK_COUNT> hook_rvas{
            0x4597C9,
            0x4597FB,
            0x45981A,
        };
        EarlyProvenanceObservation observation{};
        observation.id = m_provenance_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
        observation.frame_id = m_provenance_frames.load(std::memory_order_acquire);
        observation.writer_site_rva = hook_rvas[hook_index];
        observation.thread_id = GetCurrentThreadId();
        observation.receiver = receiver;
        observation.slot = receiver + RE4_TARGET_STATE_SLOT_OFFSET;
        observation.incoming_value = static_cast<uintptr_t>(context.rdi);
        observation.previous_readable = read_private_memory(
            observation.slot, &observation.previous_value, sizeof(observation.previous_value));
        observation.receiver_header_readable = read_private_memory(
            receiver + sizeof(uintptr_t), &observation.receiver_ref_count, sizeof(observation.receiver_ref_count));

        const bool source_owner_readable = read_memory(
            m_image_base + RE4_TARGET_STATE_SOURCE_OWNER_RVA,
            &observation.source_owner,
            sizeof(observation.source_owner));
        observation.source_slot_readable = source_owner_readable && observation.source_owner != 0 &&
            observation.source_owner <= UINTPTR_MAX - 0x60 &&
            read_private_memory(observation.source_owner + 0x60, &observation.source_value, sizeof(observation.source_value));
        if (observation.source_slot_readable) {
            observation.source_slot = observation.source_owner + 0x60;
        }

        const auto return_address_slot = static_cast<uintptr_t>(context.rsp);
        observation.caller_return_readable = return_address_slot <= UINTPTR_MAX - 0x28 &&
            read_memory(return_address_slot + 0x28, &observation.caller_return, sizeof(observation.caller_return));
        if (observation.caller_return_readable &&
            observation.caller_return >= m_image_base && observation.caller_return < m_image_end) {
            observation.caller_return_rva = static_cast<uint32_t>(observation.caller_return - m_image_base);
            if (observation.caller_return >= m_image_base + 5) {
                std::array<uint8_t, 5> call_bytes{};
                if (read_memory(observation.caller_return - 5, call_bytes.data(), call_bytes.size()) && call_bytes[0] == 0xE8) {
                    int32_t displacement{};
                    std::memcpy(&displacement, call_bytes.data() + 1, sizeof(displacement));
                    if (static_cast<int64_t>(observation.caller_return - m_image_base) + displacement ==
                        RETIRED_RE4_TARGET_STATE_WRITER_RVA) {
                        observation.caller_callsite_rva = observation.caller_return_rva - 5;
                    }
                }
            }
        }

        observation.stack_arguments_readable = return_address_slot <= UINTPTR_MAX - 0x50 &&
            read_memory(return_address_slot + 0x50,
                observation.stack_arguments.data(), sizeof(observation.stack_arguments));
        observation.registers = {
            static_cast<uintptr_t>(context.rcx),
            static_cast<uintptr_t>(context.rdx),
            static_cast<uintptr_t>(context.rbx),
            static_cast<uintptr_t>(context.rdi),
            static_cast<uintptr_t>(context.rax),
        };
        observation.direct_transfer_route = observation.receiver_header_readable && observation.receiver_ref_count < 0;

        auto& entry = m_early_provenance_observations[index];
        entry.observation = observation;
        entry.ready.store(true, std::memory_order_release);
    }

    void capture_provenance_write(size_t hook_index, const safetyhook::Context& context) noexcept {
        if (!m_provenance_active.load(std::memory_order_acquire) || hook_index >= PROVENANCE_HOOK_COUNT) {
            return;
        }

        const auto overlay_slot = m_provenance_overlay_slot.load(std::memory_order_acquire);
        const auto receiver = hook_index == 0
            ? static_cast<uintptr_t>(context.rdx)
            : static_cast<uintptr_t>(context.rbx);
        if (receiver == 0 || receiver > UINTPTR_MAX - RE4_TARGET_STATE_SLOT_OFFSET) {
            return;
        }
        if (overlay_slot == 0) {
            capture_unbound_provenance_write(hook_index, receiver, context);
            return;
        }
        if (receiver + RE4_TARGET_STATE_SLOT_OFFSET != overlay_slot) {
            return;
        }

        auto write_count = m_provenance_write_count.load(std::memory_order_acquire);
        while (write_count < MAX_PROVENANCE_WRITES) {
            if (m_provenance_write_count.compare_exchange_weak(
                    write_count, write_count + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;
            }
        }
        if (write_count >= MAX_PROVENANCE_WRITES) {
            return;
        }

        static constexpr std::array<uint32_t, PROVENANCE_HOOK_COUNT> hook_rvas{
            0x4597C9,
            0x4597FB,
            0x45981A,
        };
        const auto writer_site_rva = hook_rvas[hook_index];
        const auto incoming = static_cast<uintptr_t>(context.rdi);
        uintptr_t previous{};
        const bool previous_readable = read_private_memory(overlay_slot, &previous, sizeof(previous));
        int32_t receiver_ref_count{};
        const bool receiver_header_readable = read_private_memory(
            receiver + sizeof(uintptr_t), &receiver_ref_count, sizeof(receiver_ref_count));

        uintptr_t source_owner{};
        uintptr_t source_slot{};
        uintptr_t source_value{};
        const bool source_owner_readable = read_memory(
            m_image_base + RE4_TARGET_STATE_SOURCE_OWNER_RVA, &source_owner, sizeof(source_owner));
        const bool source_slot_readable = source_owner_readable && source_owner != 0 &&
            source_owner <= UINTPTR_MAX - 0x60 &&
            read_private_memory(source_owner + 0x60, &source_value, sizeof(source_value));
        if (source_slot_readable) {
            source_slot = source_owner + 0x60;
        }

        uintptr_t caller_return{};
        const auto return_address_slot = static_cast<uintptr_t>(context.rsp);
        const bool caller_return_readable = return_address_slot <= UINTPTR_MAX - 0x28 &&
            read_memory(return_address_slot + 0x28, &caller_return, sizeof(caller_return));
        uint32_t caller_return_rva{};
        uint32_t caller_callsite_rva{};
        if (caller_return_readable && caller_return >= m_image_base && caller_return < m_image_end) {
            caller_return_rva = static_cast<uint32_t>(caller_return - m_image_base);
            if (caller_return >= m_image_base + 5) {
                std::array<uint8_t, 5> call_bytes{};
                if (read_memory(caller_return - 5, call_bytes.data(), call_bytes.size()) && call_bytes[0] == 0xE8) {
                    int32_t displacement{};
                    std::memcpy(&displacement, call_bytes.data() + 1, sizeof(displacement));
                    if (static_cast<int64_t>(caller_return - m_image_base) + displacement ==
                        RETIRED_RE4_TARGET_STATE_WRITER_RVA) {
                        caller_callsite_rva = caller_return_rva - 5;
                    }
                }
            }
        }

        std::array<uintptr_t, 4> stack_arguments{};
        const bool stack_arguments_readable = return_address_slot <= UINTPTR_MAX - 0x50 &&
            read_memory(return_address_slot + 0x50, stack_arguments.data(), sizeof(stack_arguments));
        const auto capture_id = m_provenance_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto ownership_route = receiver_header_readable && receiver_ref_count < 0
            ? "direct-transfer-negative-owner-refcount"
            : "incoming-AddRef-then-atomic-replace-and-release-old";
        spdlog::info("[RE4XeSS][TargetStateProvenance] stage=write-attempt id={} frame={} writerSiteRva=0x{:x} writerFunctionRva=0x{:x} operation={} receiver=0x{:x} overlay=0x{:x} slot=0x{:x} previous=0x{:x} previousReadable={} incoming=0x{:x} sourceOwnerGlobalRva=0x{:x} sourceOwner=0x{:x} sourceSlot=0x{:x} sourceSlotReadable={} sourceValue=0x{:x} sourceMatchesIncoming={} receiverRefCount={} receiverHeaderReadable={} ownershipRoute={} callerReturn=0x{:x} callerReturnRva=0x{:x} callerDirectCallsiteRva=0x{:x} callerReturnReadable={} regs[rcx=0x{:x},rdx=0x{:x},rbx=0x{:x},rdi=0x{:x},rax=0x{:x}] stackArgs[0x{:x},0x{:x},0x{:x},0x{:x}] stackArgsReadable={}",
            static_cast<unsigned long long>(capture_id),
            static_cast<unsigned long long>(m_provenance_frames.load(std::memory_order_acquire)),
            writer_site_rva,
            RETIRED_RE4_TARGET_STATE_WRITER_RVA,
            hook_index == 0 ? "mov" : "lock-cmpxchg",
            receiver,
            m_provenance_overlay_object.load(std::memory_order_acquire),
            overlay_slot,
            previous,
            previous_readable,
            incoming,
            RE4_TARGET_STATE_SOURCE_OWNER_RVA,
            source_owner,
            source_slot,
            source_slot_readable,
            source_value,
            source_slot_readable && source_value == incoming,
            receiver_ref_count,
            receiver_header_readable,
            ownership_route,
            caller_return,
            caller_return_rva,
            caller_callsite_rva,
            caller_return_readable,
            static_cast<uintptr_t>(context.rcx),
            static_cast<uintptr_t>(context.rdx),
            static_cast<uintptr_t>(context.rbx),
            static_cast<uintptr_t>(context.rdi),
            static_cast<uintptr_t>(context.rax),
            stack_arguments[0], stack_arguments[1], stack_arguments[2], stack_arguments[3],
            stack_arguments_readable);

        inspect_provenance_candidate(capture_id, incoming);
        publish_pending_provenance_write({
            .id = capture_id,
            .frame_id = m_provenance_frames.load(std::memory_order_acquire),
            .slot = overlay_slot,
            .previous_value = previous,
            .incoming_value = incoming,
            .writer_site_rva = writer_site_rva,
        });
    }

    void inspect_provenance_candidate(uint64_t id, uintptr_t candidate_address) const noexcept {
        if (candidate_address == 0 || (candidate_address & (alignof(uintptr_t) - 1)) != 0) {
            spdlog::info("[RE4XeSS][TargetStateProvenance] incomingSnapshot id={} candidate=0x{:x} privateReadable=false reason=null-or-unaligned",
                static_cast<unsigned long long>(id), candidate_address);
            return;
        }

        TargetStateLayoutSnapshot candidate{};
        if (!read_private_memory(candidate_address, &candidate, sizeof(candidate))) {
            spdlog::info("[RE4XeSS][TargetStateProvenance] incomingSnapshot id={} candidate=0x{:x} privateReadable=false snapshotBytes=0x40",
                static_cast<unsigned long long>(id), candidate_address);
            return;
        }

        const auto vtable_valid = has_re4_vtable(candidate.vtable);
        const auto count_sane = candidate.num_rtv > 0 && candidate.num_rtv <= MAX_TARGET_STATE_RTVS;
        const auto rect_valid = plausible_rect(candidate);
        const auto dsv_valid = candidate.dsv == 0 || [&] {
            RenderResourceHeaderSnapshot dsv_header{};
            return inspect_render_resource_header(candidate.dsv, dsv_header);
        }();
        std::array<uintptr_t, 2> rtv_entries{};
        size_t entries_read{};
        bool rtv_array_readable{};
        if (count_sane && candidate.rtvs != 0) {
            entries_read = std::min<size_t>(candidate.num_rtv, rtv_entries.size());
            rtv_array_readable = read_private_memory(
                candidate.rtvs, rtv_entries.data(), entries_read * sizeof(uintptr_t));
        }

        RenderTargetViewSnapshot rtv0{};
        const auto rtv0_valid = rtv_array_readable && rtv_entries[0] != 0 && inspect_rtv(rtv_entries[0], rtv0);
        uintptr_t texture{};
        const bool texture_slot_readable = rtv0_valid && read_rtv_texture_pointer(rtv_entries[0], texture);
        RenderResourceHeaderSnapshot texture_header{};
        const bool texture_header_valid = texture != 0 && inspect_render_resource_header(texture, texture_header);
        const bool overlay_vtable_match = m_overlay_snapshot_valid && candidate.vtable == m_overlay_snapshot.vtable;
        const bool overlay_count_match = m_overlay_snapshot_valid && candidate.num_rtv == m_overlay_snapshot.num_rtv;
        const bool overlay_rect_match = m_overlay_snapshot_valid &&
            std::abs(candidate.rect_left - m_overlay_snapshot.rect_left) <= 1.0f &&
            std::abs(candidate.rect_top - m_overlay_snapshot.rect_top) <= 1.0f &&
            std::abs(candidate.rect_right - m_overlay_snapshot.rect_right) <= 1.0f &&
            std::abs(candidate.rect_bottom - m_overlay_snapshot.rect_bottom) <= 1.0f;
        const bool overlay_rtv0_match = m_overlay_rtv != 0 && rtv_entries[0] == m_overlay_rtv;
        RenderTargetViewSnapshot overlay_rtv0{};
        const bool overlay_rtv0_valid = m_overlay_rtv != 0 && inspect_rtv(m_overlay_rtv, overlay_rtv0);
        const bool overlay_rtv_desc_match = rtv0_valid && overlay_rtv0_valid &&
            rtv0.format == overlay_rtv0.format && rtv0.dimension == overlay_rtv0.dimension;
        const bool target_state_like = vtable_valid && overlay_vtable_match && count_sane &&
            rtv_array_readable && rtv0_valid && rect_valid && dsv_valid;

        spdlog::info("[RE4XeSS][TargetStateProvenance] incomingSnapshot id={} candidate=0x{:x} privateReadable=true snapshotBytes=0x40 vtable=0x{:x} vtableValid={} refCount={} renderFrame={} padding=0x{:x} rtvs=0x{:x} dsv=0x{:x} numRtv={} rect=({:.1f},{:.1f},{:.1f},{:.1f}) rectValid={} flag=0x{:x} countSane={} dsvValid={} overlaySnapshotValid={} overlayVtableMatch={} overlayCountMatch={} overlayRectMatch={} overlayRtv0Match={} targetStateLike={} entriesRead={}",
            static_cast<unsigned long long>(id), candidate_address,
            candidate.vtable, vtable_valid, candidate.ref_count, candidate.render_frame, candidate.padding,
            candidate.rtvs, candidate.dsv, candidate.num_rtv,
            candidate.rect_left, candidate.rect_top, candidate.rect_right, candidate.rect_bottom,
            rect_valid, candidate.flag, count_sane, dsv_valid, m_overlay_snapshot_valid,
            overlay_vtable_match, overlay_count_match, overlay_rect_match, overlay_rtv0_match,
            target_state_like, entries_read);
        spdlog::info("[RE4XeSS][TargetStateProvenance] incomingRtv id={} arrayReadable={} rtv0=0x{:x} rtv0Valid={} format={} dimension={} overlayRtv0=0x{:x} overlayRtvValid={} overlayFormat={} overlayDimension={} descMatch={} textureSlotReadable={} texture=0x{:x} textureHeaderValid={}",
            static_cast<unsigned long long>(id), rtv_array_readable, rtv_entries[0], rtv0_valid,
            rtv0.format, rtv0.dimension, m_overlay_rtv, overlay_rtv0_valid,
            overlay_rtv0.format, overlay_rtv0.dimension, overlay_rtv_desc_match,
            texture_slot_readable, texture, texture_header_valid);
    }

    void report_early_provenance_observations() noexcept {
        while (true) {
            auto index = m_early_provenance_reported_count.load(std::memory_order_acquire);
            const auto captured = std::min<uint32_t>(
                m_early_provenance_count.load(std::memory_order_acquire),
                static_cast<uint32_t>(MAX_EARLY_PROVENANCE_WRITES));
            if (index >= captured) {
                break;
            }

            auto& entry = m_early_provenance_observations[index];
            if (!entry.ready.load(std::memory_order_acquire)) {
                break;
            }
            if (!m_early_provenance_reported_count.compare_exchange_weak(
                    index, index + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }

            const auto observation = entry.observation;
            const auto overlay = m_provenance_overlay_object.load(std::memory_order_acquire);
            const auto overlay_slot = m_provenance_overlay_slot.load(std::memory_order_acquire);
            const bool correlation_candidate = overlay != 0 && overlay_slot != 0 &&
                (observation.receiver == overlay || observation.slot == overlay_slot);
            const char* ownership_route = observation.direct_transfer_route
                ? "direct-transfer-negative-owner-refcount"
                : "incoming-AddRef-then-atomic-replace-and-release-old";
            spdlog::info("[RE4XeSS][TargetStateProvenance] stage=early-write id={} frame={} tid={} writerSiteRva=0x{:x} writerFunctionRva=0x{:x} operation={} receiver=0x{:x} receiverSlot=0x{:x} previous=0x{:x} previousReadable={} incoming=0x{:x} sourceOwnerGlobalRva=0x{:x} sourceOwner=0x{:x} sourceSlot=0x{:x} sourceSlotReadable={} sourceValue=0x{:x} sourceMatchesIncoming={} receiverRefCount={} receiverHeaderReadable={} ownershipRoute={} callerReturn=0x{:x} callerReturnRva=0x{:x} callerCallsiteRva=0x{:x} callerReturnReadable={} regs[rcx=0x{:x},rdx=0x{:x},rbx=0x{:x},rdi=0x{:x},rax=0x{:x}] stackArgs[0x{:x},0x{:x},0x{:x},0x{:x}] stackArgsReadable={} overlayKnown={} correlationCandidate={}",
                static_cast<unsigned long long>(observation.id),
                static_cast<unsigned long long>(observation.frame_id),
                observation.thread_id,
                observation.writer_site_rva,
                RETIRED_RE4_TARGET_STATE_WRITER_RVA,
                observation.writer_site_rva == 0x4597C9 ? "mov" : "lock-cmpxchg",
                observation.receiver,
                observation.slot,
                observation.previous_value,
                observation.previous_readable,
                observation.incoming_value,
                RE4_TARGET_STATE_SOURCE_OWNER_RVA,
                observation.source_owner,
                observation.source_slot,
                observation.source_slot_readable,
                observation.source_value,
                observation.source_slot_readable && observation.source_value == observation.incoming_value,
                observation.receiver_ref_count,
                observation.receiver_header_readable,
                ownership_route,
                observation.caller_return,
                observation.caller_return_rva,
                observation.caller_callsite_rva,
                observation.caller_return_readable,
                observation.registers[0], observation.registers[1], observation.registers[2],
                observation.registers[3], observation.registers[4],
                observation.stack_arguments[0], observation.stack_arguments[1],
                observation.stack_arguments[2], observation.stack_arguments[3],
                observation.stack_arguments_readable,
                overlay != 0 && overlay_slot != 0,
                correlation_candidate);

            if (correlation_candidate) {
                uintptr_t current_live_state{};
                const bool current_live_state_readable = read_private_memory(
                    overlay_slot, &current_live_state, sizeof(current_live_state));
                m_early_provenance_correlated_count.fetch_add(1, std::memory_order_acq_rel);
                spdlog::info("[RE4XeSS][TargetStateProvenance] stage=early-correlation id={} receiver=0x{:x} overlay=0x{:x} slot=0x{:x} previous=0x{:x} incoming=0x{:x} currentLiveTargetState=0x{:x} currentLiveReadable={} incomingMatchesCurrent={} sourceValue=0x{:x} sourceMatchesIncoming={} writerSiteRva=0x{:x} callerCallsiteRva=0x{:x} ownershipRoute={}",
                    static_cast<unsigned long long>(observation.id),
                    observation.receiver, overlay, overlay_slot,
                    observation.previous_value, observation.incoming_value,
                    current_live_state, current_live_state_readable,
                    current_live_state_readable && observation.incoming_value == current_live_state,
                    observation.source_value,
                    observation.source_slot_readable && observation.source_value == observation.incoming_value,
                    observation.writer_site_rva, observation.caller_callsite_rva, ownership_route);
                inspect_provenance_candidate(observation.id, observation.incoming_value);
            }
        }

        if (m_early_provenance_overflow.load(std::memory_order_acquire) &&
            !m_early_provenance_overflow_logged.exchange(true, std::memory_order_acq_rel)) {
            spdlog::warn("[RE4XeSS][TargetStateProvenance] early observation buffer full capacity={} additionalUnboundWritesDropped=true",
                MAX_EARLY_PROVENANCE_WRITES);
        }
    }

    void publish_pending_provenance_write(const PendingProvenanceWrite& pending) noexcept {
        int expected{};
        if (!m_provenance_pending_state.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (!m_provenance_pending_overflow_logged.exchange(true, std::memory_order_acq_rel)) {
                spdlog::warn("[RE4XeSS][TargetStateProvenance] pending assignment observation already occupied; later write confirmations are suppressed until it resolves");
            }
            return;
        }
        m_pending_provenance_write = pending;
        m_provenance_pending_state.store(2, std::memory_order_release);
    }

    void poll_pending_provenance_write() noexcept {
        int expected = 2;
        if (!m_provenance_pending_state.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        auto pending = m_pending_provenance_write;
        uintptr_t current_value{};
        const bool current_readable = read_private_memory(pending.slot, &current_value, sizeof(current_value));
        std::string_view result;
        if (current_readable && current_value != pending.previous_value && current_value == pending.incoming_value) {
            result = "assignment-observed";
            m_provenance_confirmed_count.fetch_add(1, std::memory_order_acq_rel);
        } else if (current_readable && current_value != pending.previous_value) {
            result = "slot-changed-to-different-value";
        } else if (++pending.frames_waited >= MAX_PROVENANCE_PENDING_FRAMES) {
            result = current_readable ? "slot-did-not-change-in-observation-window" : "slot-unreadable-in-observation-window";
        }

        const bool complete = !result.empty();
        if (complete) {
            m_provenance_pending_state.store(0, std::memory_order_release);
        } else {
            m_pending_provenance_write = pending;
            m_provenance_pending_state.store(2, std::memory_order_release);
        }
        if (complete) {
            spdlog::info("[RE4XeSS][TargetStateProvenance] stage={} id={} frame={} writerSiteRva=0x{:x} slot=0x{:x} previous=0x{:x} incoming=0x{:x} observed=0x{:x} observedReadable={} framesWaited={}",
                result,
                static_cast<unsigned long long>(pending.id),
                static_cast<unsigned long long>(pending.frame_id),
                pending.writer_site_rva,
                pending.slot,
                pending.previous_value,
                pending.incoming_value,
                current_value,
                current_readable,
                pending.frames_waited);
        }
    }

    static constexpr bool selected_site(size_t site) noexcept {
        return ISOLATED_SITE_INDEX >= SITE_COUNT || site == ISOLATED_SITE_INDEX;
    }

    static constexpr bool selected_hook(size_t hook_index) noexcept {
        const auto site = hook_index < SITE_COUNT ? hook_index : hook_index - SITE_COUNT;
        return site < SITE_COUNT && selected_site(site);
    }

    uintptr_t resolve_callee(size_t site, const safetyhook::Context& context) const noexcept {
        if (site == 0) {
            return m_image_base + 0x4470470;
        }

        uintptr_t receiver{};
        uintptr_t slot_offset{};
        if (site == 1) {
            receiver = static_cast<uintptr_t>(context.rdi);
            slot_offset = 0x40;
        } else if (site == 2) {
            receiver = static_cast<uintptr_t>(context.r14);
            slot_offset = 0x40;
        } else if (site == 3) {
            const auto vtable = read_pointer(static_cast<uintptr_t>(context.r14));
            if (vtable == 0 || vtable > UINTPTR_MAX - 0xC06C08) {
                return 0;
            }
            receiver = read_pointer(vtable + 0xC06C08);
            slot_offset = 0xA0;
        } else {
            return 0;
        }

        const auto vtable = read_pointer(receiver);
        if (vtable == 0 || vtable > UINTPTR_MAX - slot_offset) {
            return 0;
        }
        uintptr_t target{};
        return read_memory(vtable + slot_offset, &target, sizeof(target)) ? target : 0;
    }

    bool reserve_call(size_t site, uint64_t& id) noexcept {
        if (site >= SITE_COUNT) {
            return false;
        }

        auto site_count = m_site_call_counts[site].load(std::memory_order_acquire);
        while (site_count < MAX_CAPTURED_CALLS_PER_SITE) {
            if (m_site_call_counts[site].compare_exchange_weak(
                    site_count, site_count + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                const auto total_count = m_calls_captured.fetch_add(1, std::memory_order_acq_rel);
                if (total_count >= MAX_CAPTURED_CALLS) {
                    m_calls_captured.fetch_sub(1, std::memory_order_acq_rel);
                    m_site_call_counts[site].fetch_sub(1, std::memory_order_acq_rel);
                    m_capture_stopped.store(true, std::memory_order_release);
                    return false;
                }
                id = m_next_call_id.fetch_add(1, std::memory_order_relaxed) + 1;
                return true;
            }
        }
        return false;
    }

    static PendingObservation* find_pending_observation(size_t site, uintptr_t stack_pointer) noexcept {
        for (auto& pending : s_pending_observations) {
            if (pending.active && pending.site == site && pending.stack_pointer == stack_pointer) {
                return &pending;
            }
        }
        return nullptr;
    }

    static bool read_stack_window(uintptr_t rsp, std::array<uintptr_t, 4>& values) noexcept {
        if (rsp == 0 || rsp > UINTPTR_MAX - 0x40) {
            return false;
        }
        return read_memory(rsp + 0x20, values.data(), sizeof(values));
    }

    CallArguments reconstruct_arguments(size_t site, const safetyhook::Context& context) const noexcept {
        CallArguments args{};
        if (site == 0) {
            args.rcx = static_cast<uintptr_t>(context.r12);
            args.rdx = static_cast<uintptr_t>(context.rbx);
            const auto rbp = static_cast<uintptr_t>(context.rbp);
            if (rbp != 0 && rbp <= UINTPTR_MAX - 0x6D0) {
                args.r8 = rbp + 0x6D0;
                args.valid_mask = 0x07;
            } else {
                args.valid_mask = 0x03;
            }
            return args;
        }
        if (site == 1) {
            args.rcx = static_cast<uintptr_t>(context.rdi);
            args.valid_mask = 0x01;
            return args;
        }
        if (site == 2) {
            args.rcx = static_cast<uintptr_t>(context.r14);
            args.valid_mask = 0x01;
            return args;
        }
        if (site != 3) {
            return args;
        }

        const auto r14 = static_cast<uintptr_t>(context.r14);
        const auto rsi = static_cast<uintptr_t>(context.rsi);
        const auto rbp = static_cast<uintptr_t>(context.rbp);
        const auto r15 = static_cast<uintptr_t>(context.r15);
        const auto vtable = read_pointer(r14);
        if (vtable != 0 && vtable <= UINTPTR_MAX - 0xC06C08) {
            args.rcx = read_pointer(vtable + 0xC06C08);
            if (args.rcx != 0) args.valid_mask |= 0x01;
        }
        if (rsi != 0 && rsi <= UINTPTR_MAX - 0xB8) {
            const auto desc_owner = read_pointer(rsi + 0xB8);
            if (desc_owner != 0 && desc_owner <= UINTPTR_MAX - 0x18) {
                args.rdx = read_pointer(desc_owner + 0x18);
                if (args.rdx != 0) args.valid_mask |= 0x02;
            }
        }
        if (rbp >= 0x30) {
            args.r8 = rbp - 0x30;
            args.valid_mask |= 0x04;
        }
        args.r9 = r15;
        args.valid_mask |= 0x08;
        return args;
    }

    std::string format_rva(uintptr_t address) const {
        std::ostringstream text;
        text << std::hex;
        if (address >= m_image_base && address < m_image_end) {
            text << "re4+0x" << (address - m_image_base);
        } else {
            text << "0x" << address;
        }
        return text.str();
    }

    std::string describe_private_words(uintptr_t address) const {
        if (address == 0 || (address & (alignof(uintptr_t) - 1)) != 0) {
            return "not-pointer";
        }

        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return "not-private-readable";
        }

        std::array<uintptr_t, 3> words{};
        if (!read_memory(address, words.data(), sizeof(words))) {
            return "private-read-failed";
        }

        const auto identity_mask = [this](uintptr_t value) {
            return (value == m_overlay_state ? 0x01 : 0) |
                (value == m_overlay_desc ? 0x02 : 0) |
                (value == m_overlay_rtv_array ? 0x04 : 0) |
                (value == m_overlay_rtv ? 0x08 : 0) |
                (value == m_semantic_color ? 0x10 : 0);
        };

        std::ostringstream text;
        text << std::hex << "[0x" << words[0] << "/m0x" << identity_mask(words[0])
             << ",0x" << words[1] << "/m0x" << identity_mask(words[1])
             << ",0x" << words[2] << "/m0x" << identity_mask(words[2]) << ']';
        return text.str();
    }

    bool is_re4_executable(uintptr_t address) const noexcept {
        if (address < m_image_base || address >= m_image_end) {
            return false;
        }

        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }

        const auto protection = memory.Protect & 0xFF;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }

    bool has_re4_vtable(uintptr_t vtable) const noexcept {
        if (vtable < m_image_base || vtable >= m_image_end || vtable > UINTPTR_MAX - sizeof(uintptr_t)) {
            return false;
        }
        return is_re4_executable(read_pointer(vtable));
    }

    bool plausible_rect(const TargetStateLayoutSnapshot& snapshot) const noexcept {
        const std::array<float, 4> rect{
            snapshot.rect_left, snapshot.rect_top, snapshot.rect_right, snapshot.rect_bottom};
        if (!std::all_of(rect.begin(), rect.end(), [](float value) {
                return std::isfinite(value) && std::abs(value) <= 65536.0f;
            })) {
            return false;
        }

        const auto width = snapshot.rect_right - snapshot.rect_left;
        const auto height = snapshot.rect_bottom - snapshot.rect_top;
        return width > 0.0f && height > 0.0f && width <= 32768.0f && height <= 32768.0f;
    }

    bool inspect_render_resource_header(uintptr_t address, RenderResourceHeaderSnapshot& header) const noexcept {
        return read_private_memory(address, &header, sizeof(header)) && has_re4_vtable(header.vtable);
    }

    bool inspect_rtv(uintptr_t address, RenderTargetViewSnapshot& rtv) const noexcept {
        if (!read_private_memory(address, &rtv, sizeof(rtv)) || !has_re4_vtable(rtv.header.vtable)) {
            return false;
        }
        return rtv.format != 0 && rtv.dimension >= D3D12_RTV_DIMENSION_BUFFER &&
            rtv.dimension <= D3D12_RTV_DIMENSION_TEXTURE3D;
    }

    bool read_rtv_texture_pointer(uintptr_t address, uintptr_t& texture) const noexcept {
        texture = 0;
        if (address > UINTPTR_MAX - 0x1000 || !is_private_readable(address, sizeof(RenderTargetViewSnapshot))) {
            return false;
        }

        try {
            if (reframework::get_types() == nullptr) {
                return false;
            }
            auto& texture_ref = reinterpret_cast<sdk::renderer::RenderTargetView*>(address)->get_texture_d3d12();
            const auto slot = reinterpret_cast<uintptr_t>(&texture_ref);
            return read_private_memory(slot, &texture, sizeof(texture));
        } catch (...) {
            return false;
        }
    }

    void inspect_provider_return(uint64_t id, uintptr_t candidate_address) const noexcept {
        if (candidate_address == 0 || (candidate_address & (alignof(uintptr_t) - 1)) != 0) {
            spdlog::info("[RE4XeSS][TargetStateProbe] providerReturn id={} callee=re4+0x{:x} rax=0x{:x} privateReadable=false reason=null-or-unaligned",
                static_cast<unsigned long long>(id), RE4_PROVIDER_CALLEE_RVA, candidate_address);
            return;
        }

        TargetStateLayoutSnapshot candidate{};
        if (!read_private_memory(candidate_address, &candidate, sizeof(candidate))) {
            spdlog::info("[RE4XeSS][TargetStateProbe] providerReturn id={} callee=re4+0x{:x} rax=0x{:x} privateReadable=false snapshotBytes=0x40",
                static_cast<unsigned long long>(id), RE4_PROVIDER_CALLEE_RVA, candidate_address);
            return;
        }

        const auto vtable_valid = has_re4_vtable(candidate.vtable);
        const auto count_sane = candidate.num_rtv > 0 && candidate.num_rtv <= MAX_TARGET_STATE_RTVS;
        const auto rect_valid = plausible_rect(candidate);
        const auto dsv_valid = candidate.dsv == 0 || [&] {
            RenderResourceHeaderSnapshot dsv_header{};
            return inspect_render_resource_header(candidate.dsv, dsv_header);
        }();

        std::array<uintptr_t, 2> rtv_entries{};
        size_t entries_read{};
        bool rtv_array_readable{};
        if (count_sane && candidate.rtvs != 0) {
            entries_read = std::min<size_t>(candidate.num_rtv, rtv_entries.size());
            const auto bytes = entries_read * sizeof(uintptr_t);
            rtv_array_readable = read_private_memory(candidate.rtvs, rtv_entries.data(), bytes);
        }

        RenderTargetViewSnapshot rtv0{};
        const auto rtv0_valid = rtv_array_readable && rtv_entries[0] != 0 && inspect_rtv(rtv_entries[0], rtv0);
        uintptr_t texture{};
        const auto texture_slot_readable = rtv0_valid && read_rtv_texture_pointer(rtv_entries[0], texture);
        RenderResourceHeaderSnapshot texture_header{};
        const auto texture_header_valid = texture != 0 && inspect_render_resource_header(texture, texture_header);

        const auto overlay_vtable_match = m_overlay_snapshot_valid && candidate.vtable == m_overlay_snapshot.vtable;
        const auto overlay_rtv_count_match = m_overlay_snapshot_valid && candidate.num_rtv == m_overlay_snapshot.num_rtv;
        const auto overlay_rtv0_match = m_overlay_rtv != 0 && rtv_entries[0] == m_overlay_rtv;
        RenderTargetViewSnapshot overlay_rtv0{};
        const auto overlay_rtv0_valid = m_overlay_rtv != 0 && inspect_rtv(m_overlay_rtv, overlay_rtv0);
        const auto overlay_rtv_desc_match = rtv0_valid && overlay_rtv0_valid &&
            rtv0.format == overlay_rtv0.format && rtv0.dimension == overlay_rtv0.dimension;
        const auto overlay_rect_match = m_overlay_snapshot_valid &&
            std::abs(candidate.rect_left - m_overlay_snapshot.rect_left) <= 1.0f &&
            std::abs(candidate.rect_top - m_overlay_snapshot.rect_top) <= 1.0f &&
            std::abs(candidate.rect_right - m_overlay_snapshot.rect_right) <= 1.0f &&
            std::abs(candidate.rect_bottom - m_overlay_snapshot.rect_bottom) <= 1.0f;

        const bool target_state_like = vtable_valid && overlay_vtable_match && count_sane &&
            rtv_array_readable && rtv0_valid && rect_valid && dsv_valid;

        spdlog::info("[RE4XeSS][TargetStateProbe] providerReturn id={} callee=re4+0x{:x} rax=0x{:x} privateReadable=true snapshotBytes=0x40 vtable=0x{:x} vtableValid={} refCount={} renderFrame={} padding=0x{:x} rtvs=0x{:x} dsv=0x{:x} numRtv={} rect=({:.1f},{:.1f},{:.1f},{:.1f}) rectValid={} flag=0x{:x} countSane={} dsvValid={} overlaySnapshotValid={} overlayVtableMatch={} overlayCountMatch={} overlayRectMatch={} overlayRtv0Match={} targetStateLike={} entriesRead={}",
            static_cast<unsigned long long>(id), RE4_PROVIDER_CALLEE_RVA, candidate_address,
            candidate.vtable, vtable_valid, candidate.ref_count, candidate.render_frame, candidate.padding,
            candidate.rtvs, candidate.dsv, candidate.num_rtv,
            candidate.rect_left, candidate.rect_top, candidate.rect_right, candidate.rect_bottom,
            rect_valid, candidate.flag, count_sane, dsv_valid, m_overlay_snapshot_valid,
            overlay_vtable_match, overlay_rtv_count_match, overlay_rect_match, overlay_rtv0_match,
            target_state_like, entries_read);

        spdlog::info("[RE4XeSS][TargetStateProbe] providerRtv id={} arrayReadable={} rtv0=0x{:x} rtv0Valid={} format={} dimension={} overlayRtv0=0x{:x} overlayRtvValid={} overlayFormat={} overlayDimension={} descMatch={} textureSlotReadable={} texture=0x{:x} textureHeaderValid={}",
            static_cast<unsigned long long>(id), rtv_array_readable, rtv_entries[0], rtv0_valid,
            rtv0.format, rtv0.dimension, m_overlay_rtv, overlay_rtv0_valid,
            overlay_rtv0.format, overlay_rtv0.dimension, overlay_rtv_desc_match,
            texture_slot_readable, texture, texture_header_valid);
    }

    std::string identity_matches(uintptr_t value) const {
        std::ostringstream text;
        text << "state=" << (value == m_overlay_state)
             << ",desc=" << (value == m_overlay_desc)
             << ",rtvArray=" << (value == m_overlay_rtv_array)
             << ",rtv0=" << (value == m_overlay_rtv)
             << ",color=" << (value == m_semantic_color);
        return text.str();
    }

    void log_overlay_state_snapshot() const {
        if (!m_overlay_snapshot_valid) {
            spdlog::warn("[RE4XeSS][TargetStateProbe] overlaySnapshot unavailable state=0x{:x} bytes=0x40 privateReadable=false",
                m_overlay_state);
            return;
        }
        spdlog::info("[RE4XeSS][TargetStateProbe] overlaySnapshot overlay=0x{:x} slot=0x{:x} state=0x{:x} desc=0x{:x} vtable=0x{:x} refCount={} renderFrame={} padding=0x{:x} rtvArray=0x{:x} dsv=0x{:x} count={} rtv0=0x{:x} rect=({:.1f},{:.1f},{:.1f},{:.1f}) flag=0x{:x} color=0x{:x}",
            m_provenance_overlay_object.load(std::memory_order_acquire),
            m_provenance_overlay_slot.load(std::memory_order_acquire),
            m_overlay_state,
            m_overlay_desc,
            m_overlay_snapshot.vtable,
            m_overlay_snapshot.ref_count,
            m_overlay_snapshot.render_frame,
            m_overlay_snapshot.padding,
            m_overlay_rtv_array,
            m_overlay_snapshot.dsv,
            m_overlay_snapshot.num_rtv,
            m_overlay_rtv,
            m_overlay_snapshot.rect_left,
            m_overlay_snapshot.rect_top,
            m_overlay_snapshot.rect_right,
            m_overlay_snapshot.rect_bottom,
            m_overlay_snapshot.flag,
            m_semantic_color);
    }

    void log_observation(
        std::string_view stage,
        uint64_t id,
        size_t site,
        const safetyhook::Context& context,
        const CallArguments& args,
        uintptr_t callee) const {
        std::array<uintptr_t, 4> stack{};
        const auto has_stack = read_stack_window(static_cast<uintptr_t>(context.rsp), stack);
        const auto rax = static_cast<uintptr_t>(context.rax);
        const auto tid = GetCurrentThreadId();
        spdlog::info("[RE4XeSS][TargetStateProbe] stage={} id={} armFrame={} site={} callsiteRva=0x{:x} returnRva=0x{:x} callee={} tid={} armThread={} rsp=0x{:x} regs[rax=0x{:x},rcx=0x{:x},rdx=0x{:x},r8=0x{:x},r9=0x{:x},rdi=0x{:x},rsi=0x{:x},r12=0x{:x},r14=0x{:x},r15=0x{:x},rbp=0x{:x}] abiMask=0x{:x} abiArgs[rcx=0x{:x},rdx=0x{:x},r8=0x{:x},r9=0x{:x}] stack20=[0x{:x},0x{:x},0x{:x},0x{:x}] stackValid={} identity[abiRCX:{};abiRDX:{};abiR8:{};abiR9:{};rax:{}] words[abiRCX={};abiRDX={};abiR8={};abiR9={};rax={}]",
            stage,
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(m_frame_id),
            SITE_NAMES[site],
            CALLSITE_RVAS[site],
            RETURN_RVAS[site],
            format_rva(callee),
            tid,
            m_arm_thread_id,
            static_cast<uintptr_t>(context.rsp),
            rax,
            static_cast<uintptr_t>(context.rcx),
            static_cast<uintptr_t>(context.rdx),
            static_cast<uintptr_t>(context.r8),
            static_cast<uintptr_t>(context.r9),
            static_cast<uintptr_t>(context.rdi),
            static_cast<uintptr_t>(context.rsi),
            static_cast<uintptr_t>(context.r12),
            static_cast<uintptr_t>(context.r14),
            static_cast<uintptr_t>(context.r15),
            static_cast<uintptr_t>(context.rbp),
            args.valid_mask,
            args.rcx,
            args.rdx,
            args.r8,
            args.r9,
            stack[0],
            stack[1],
            stack[2],
            stack[3],
            has_stack,
            identity_matches(args.rcx),
            identity_matches(args.rdx),
            identity_matches(args.r8),
            identity_matches(args.r9),
            identity_matches(rax),
            describe_private_words(args.rcx),
            describe_private_words(args.rdx),
            describe_private_words(args.r8),
            describe_private_words(args.r9),
            describe_private_words(rax));
    }

    void capture_before(size_t site, const safetyhook::Context& context) noexcept {
        if (!m_active.load(std::memory_order_acquire) || !m_capture_started.load(std::memory_order_acquire) ||
            m_capture_stopped.load(std::memory_order_acquire) || site >= SITE_COUNT) {
            return;
        }

        try {
            auto pending = std::find_if(s_pending_observations.begin(), s_pending_observations.end(),
                [](const PendingObservation& observation) { return !observation.active; });
            if (pending == s_pending_observations.end()) {
                m_capture_stopped.store(true, std::memory_order_release);
                spdlog::warn("[RE4XeSS][TargetStateProbe] capture stopped: per-thread pending-call table full tid={} site={} rsp=0x{:x}",
                    GetCurrentThreadId(), SITE_NAMES[site], static_cast<uintptr_t>(context.rsp));
                return;
            }

            uint64_t id{};
            if (!reserve_call(site, id)) {
                return;
            }

            const auto arguments = reconstruct_arguments(site, context);
            const auto callee = resolve_callee(site, context);
            *pending = PendingObservation{
                .active = true,
                .site = site,
                .stack_pointer = static_cast<uintptr_t>(context.rsp),
                .id = id,
                .arguments = arguments,
                .callee = callee,
            };
            m_pending_call_count.fetch_add(1, std::memory_order_acq_rel);
            log_observation("pre", id, site, context, arguments, callee);

            if (m_calls_captured.load(std::memory_order_acquire) >= MAX_CAPTURED_CALLS) {
                m_capture_stopped.store(true, std::memory_order_release);
            }
        } catch (...) {
            // Probe failures must not affect the original engine call.
        }
    }

    void capture_after(size_t site, const safetyhook::Context& context) noexcept {
        if (!m_active.load(std::memory_order_acquire) || !m_capture_started.load(std::memory_order_acquire) ||
            site >= SITE_COUNT) {
            return;
        }

        try {
            m_return_hook_entries[site].fetch_add(1, std::memory_order_acq_rel);
            const auto stack_pointer = static_cast<uintptr_t>(context.rsp);
            auto pending = find_pending_observation(site, stack_pointer);
            if (pending == nullptr) {
                const auto unmatched = m_unmatched_returns[site].fetch_add(1, std::memory_order_acq_rel);
                if (unmatched < MAX_CAPTURED_CALLS_PER_SITE) {
                    uint32_t same_thread_pending{};
                    uintptr_t expected_rsp{};
                    uint64_t expected_id{};
                    for (const auto& candidate : s_pending_observations) {
                        if (candidate.active && candidate.site == site) {
                            if (same_thread_pending == 0) {
                                expected_rsp = candidate.stack_pointer;
                                expected_id = candidate.id;
                            }
                            ++same_thread_pending;
                        }
                    }
                    spdlog::warn("[RE4XeSS][TargetStateProbe] post-hook unmatched site={} callsiteRva=0x{:x} tid={} rsp=0x{:x} sameThreadPending={} expectedId={} expectedRsp=0x{:x} pendingCalls={}",
                        SITE_NAMES[site], CALLSITE_RVAS[site], GetCurrentThreadId(), stack_pointer,
                        same_thread_pending, expected_id, expected_rsp,
                        m_pending_call_count.load(std::memory_order_acquire));
                }
                return;
            }

            const auto observation = *pending;
            pending->active = false;
            m_pending_call_count.fetch_sub(1, std::memory_order_acq_rel);
            m_site_return_counts[site].fetch_add(1, std::memory_order_acq_rel);
            m_returns_captured.fetch_add(1, std::memory_order_acq_rel);
            log_observation("post", observation.id, site, context, observation.arguments, observation.callee);
            if (site == ISOLATED_SITE_INDEX && observation.callee == m_image_base + RE4_PROVIDER_CALLEE_RVA) {
                inspect_provider_return(observation.id, static_cast<uintptr_t>(context.rax));
            }
        } catch (...) {
            // Probe failures must not affect the original engine call.
        }
    }

    static uintptr_t read_pointer(uintptr_t address) noexcept {
        uintptr_t value{};
        return read_memory(address, &value, sizeof(value)) ? value : 0;
    }
};

TargetStateFactoryProbe::CreatorObjectSnapshot TargetStateFactoryProbe::snapshot_creator_object(
    uintptr_t object) const noexcept {
    CreatorObjectSnapshot result{};
    result.object = object;
    if (object == 0) {
        return result;
    }

    TargetStateLayoutSnapshot state{};
    if (!read_private_memory(object, &state, sizeof(state)) || !has_re4_vtable(state.vtable)) {
        return result;
    }
    result.readable = true;
    result.vtable = state.vtable;
    result.ref_count = state.ref_count;
    result.render_frame = state.render_frame;
    result.rtvs = state.rtvs;
    result.dsv = state.dsv;
    result.num_rtv = state.num_rtv;
    result.rect_left = state.rect_left;
    result.rect_top = state.rect_top;
    result.rect_right = state.rect_right;
    result.rect_bottom = state.rect_bottom;
    result.flag = state.flag;

    if (state.rtvs != 0 && state.num_rtv != 0 && state.num_rtv <= MAX_TARGET_STATE_RTVS) {
        if (read_private_memory(state.rtvs, &result.rtv0, sizeof(result.rtv0)) && result.rtv0 != 0) {
            RenderTargetViewSnapshot rtv{};
            result.rtv_valid = inspect_rtv(result.rtv0, rtv);
            if (result.rtv_valid) {
                result.rtv_format = rtv.format;
                result.rtv_dimension = rtv.dimension;
                if (read_rtv_texture_pointer(result.rtv0, result.texture)) {
                    RenderResourceHeaderSnapshot texture_header{};
                    result.texture_header_valid = inspect_render_resource_header(result.texture, texture_header);
                    if (result.texture_header_valid) {
                        result.texture_vtable = texture_header.vtable;
                        result.texture_ref_count = texture_header.ref_count;
                        result.texture_render_frame = texture_header.render_frame;
                    }
                }
            }
        }
    }
    result.target_state_like = state.vtable == m_expected_vtable.load(std::memory_order_acquire) &&
        state.num_rtv == 1 && plausible_rect(state) && result.rtv_valid;
    return result;
}

bool is_valid_texture_extent(ID3D12Resource* resource, uint32_t width, uint32_t height) {
    if (resource == nullptr) {
        return false;
    }

    const auto description = resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == width && description.Height == height &&
        description.SampleDesc.Count == 1;
}

std::filesystem::path reframework_module_directory() {
    const auto module = REFramework::get_reframework_module();
    if (module == nullptr) {
        return {};
    }
    const auto module_path = utility::get_module_path(module);
    return module_path ? std::filesystem::path{ *module_path }.parent_path() : std::filesystem::path{};
}

}

RE4XeSS::~RE4XeSS() {
    m_worker.stop();
    log_re4_xess_lifetime_summary();
}

RE4XeSS::LoadStateUpdateScope::LoadStateUpdateScope(RE4XeSS& owner)
    : m_owner(owner) {
    const auto active_before = m_owner.m_load_state_update_active_count.fetch_add(
        1, std::memory_order_acq_rel);
    if (active_before != 0) {
        m_owner.m_load_state_update_overlap_count.fetch_add(1, std::memory_order_relaxed);
    }

    try {
        m_lock = std::unique_lock<std::mutex>{ m_owner.m_load_state_mutex };
    } catch (...) {
        m_owner.m_load_state_update_active_count.fetch_sub(1, std::memory_order_release);
        throw;
    }
    m_owner.m_load_state_update_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
}

RE4XeSS::LoadStateUpdateScope::~LoadStateUpdateScope() noexcept {
    const RE4XeSSLoadEligibility::Snapshot snapshot{
        m_owner.m_load_observation_valid,
        m_owner.m_pause_previous_valid,
        m_owner.m_pause_previous,
        m_owner.m_remembered_normal_inhibit_valid,
        m_owner.m_inhibit_departure_pending,
        m_owner.m_load_transition_active,
        m_owner.m_startup_mid_load,
    };
    m_owner.m_published_load_state_bits.store(
        RE4XeSSLoadEligibility::encode(snapshot), std::memory_order_release);
    m_owner.m_load_state_update_sequence.fetch_add(1, std::memory_order_release);
    m_owner.m_load_state_update_active_count.fetch_sub(1, std::memory_order_release);
}

RE4XeSSLoadEligibility::UpdateWindow RE4XeSS::begin_load_state_trace() const noexcept {
    RE4XeSSLoadEligibility::UpdateWindow window{};
    window.sequence_before = m_load_state_update_sequence.load(std::memory_order_acquire);
    window.active_before = m_load_state_update_active_count.load(std::memory_order_acquire);
    window.update_thread_id = m_load_state_update_thread_id.load(std::memory_order_acquire);
    window.overlap_count = m_load_state_update_overlap_count.load(std::memory_order_acquire);
    window.state = RE4XeSSLoadEligibility::decode(
        m_published_load_state_bits.load(std::memory_order_acquire));
    return window;
}

void RE4XeSS::finish_load_state_trace(RE4XeSSLoadEligibility::UpdateWindow& window) const noexcept {
    window.active_after = m_load_state_update_active_count.load(std::memory_order_acquire);
    window.sequence_after = m_load_state_update_sequence.load(std::memory_order_acquire);
    window.overlap_count = m_load_state_update_overlap_count.load(std::memory_order_acquire);
}

void RE4XeSS::set_load_state_trace_fields(
    RE4XeSSLifetimeTrace::Event& event,
    const RE4XeSSLoadEligibility::UpdateWindow& window) const noexcept {
    event.load_state_update_sequence_before = window.sequence_before;
    event.load_state_update_sequence_after = window.sequence_after;
    event.load_state_update_overlap_count = window.overlap_count;
    event.load_state_update_thread_id = window.update_thread_id;
    event.load_state_update_active_before = window.active_before;
    event.load_state_update_active_after = window.active_after;
    event.load_state_flags = RE4XeSSLoadEligibility::encode(window.state);
    event.load_state_observation_valid = window.state.observation_valid;
    event.load_state_admitted = RE4XeSSLoadEligibility::allows_temporal_rendering(window.state);
    event.load_state_update_overlapped = window.overlapped();
}

void RE4XeSS::trace_load_state_admission(
    uint32_t callback_kind,
    std::string_view reason,
    const RE4XeSSLoadEligibility::UpdateWindow& window) {
    auto& trace = RE4XeSSLifetimeTrace::instance();
    if (!trace.enabled() || callback_kind >= m_last_load_admission_signatures.size()) {
        return;
    }

    uint64_t signature = 14695981039346656037ull;
    const auto mix = [&signature](uint64_t value) {
        for (uint32_t byte = 0; byte < 8; ++byte) {
            signature ^= static_cast<uint8_t>(value >> (byte * 8));
            signature *= 1099511628211ull;
        }
    };
    mix(callback_kind);
    mix(GetCurrentThreadId());
    mix(window.update_thread_id);
    mix(RE4XeSSLoadEligibility::encode(window.state));
    mix(RE4XeSSLoadEligibility::allows_temporal_rendering(window.state));
    mix(window.overlapped());
    for (const auto character : reason) {
        signature ^= static_cast<uint8_t>(character);
        signature *= 1099511628211ull;
    }
    if (signature == 0) {
        signature = 1;
    }
    if (m_last_load_admission_signatures[callback_kind].exchange(
            signature, std::memory_order_acq_rel) == signature) {
        return;
    }

    auto emitted = m_load_admission_event_count.load(std::memory_order_relaxed);
    for (;;) {
        if (emitted >= 32) {
            return;
        }
        if (m_load_admission_event_count.compare_exchange_weak(
                emitted, emitted + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            break;
        }
    }

    RE4XeSSLifetimeTrace::Event event{};
    event.kind = RE4XeSSLifetimeTrace::Kind::LoadAdmission;
    event.load_state_callback_kind = callback_kind;
    event.thread_id = GetCurrentThreadId();
    event.control_generation = m_control_generation.load(std::memory_order_acquire);
    event.device_reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
    if (const auto* renderer = sdk::renderer::get_renderer(); renderer != nullptr) {
        if (const auto frame = renderer->get_render_frame(); frame.has_value()) {
            event.frame_id = static_cast<uint64_t>(*frame);
            event.frame_valid = true;
        }
    }
    set_load_state_trace_fields(event, window);
    RE4XeSSLifetimeTrace::set_reason(event, reason);
    trace.record(event);

    spdlog::info(
        "[RE4XeSS][LoadAdmission] callback={} frame={} frameValid={} tid={} updateTid={} sequence={}->{} active={}->{} overlaps={} flags=0x{:x} observed={} admitted={} updateOverlapped={} reason={}",
        load_state_callback_name(callback_kind),
        static_cast<unsigned long long>(event.frame_id),
        event.frame_valid,
        event.thread_id,
        event.load_state_update_thread_id,
        static_cast<unsigned long long>(event.load_state_update_sequence_before),
        static_cast<unsigned long long>(event.load_state_update_sequence_after),
        event.load_state_update_active_before,
        event.load_state_update_active_after,
        static_cast<unsigned long long>(event.load_state_update_overlap_count),
        event.load_state_flags,
        event.load_state_observation_valid,
        event.load_state_admitted,
        event.load_state_update_overlapped,
        reason);
}

std::optional<std::string> RE4XeSS::on_initialize() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return std::nullopt;
    }

    std::string error;
    if (!m_worker.start(error)) {
        const std::string reason = error.empty() ? "The RE4XeSS worker could not be started" : error;
        set_owner_unavailable(reason, false, true);
        spdlog::error("[RE4XeSS][Failure] {}", reason);
    }
    return std::nullopt;
}

std::optional<std::string> RE4XeSS::on_initialize_d3d_thread() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return std::nullopt;
    }

    const bool debug_log = REFrameworkConfig::get() != nullptr &&
        REFrameworkConfig::get()->is_debug_log_enabled();
    if (g_framework != nullptr &&
        g_framework->get_renderer_type() == REFramework::RendererType::D3D12) {
        const auto& hook = g_framework->get_d3d12_hook();
        if (hook != nullptr) {
            hook->set_present_diagnostics_enabled(debug_log);
        }
    }
    return std::nullopt;
}

void RE4XeSS::on_frame() {
    clear_frame_state();

    if (!sdk::GameIdentity::get().is_re4()) {
        reset_temporal_state("non-re4", true);
        return;
    }

    const auto reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
    if (reset_generation != m_last_frame_device_reset_generation.load(std::memory_order_acquire)) {
        m_last_frame_device_reset_generation.store(reset_generation, std::memory_order_release);
        reset_temporal_state("d3d12-device-reset", false);
    }

    const auto requested_mode = m_requested_mode.load(std::memory_order_acquire);
    if (requested_mode != m_last_frame_mode) {
        m_last_frame_mode = requested_mode;
        reset_temporal_state(requested_mode == UpscalingMode::Off ? "mode-off" : "mode-change",
            requested_mode == UpscalingMode::Off);
    }

    if (requested_mode == UpscalingMode::Off) {
        return;
    }

    update_load_state();
    update_temporal_configuration();
}

void RE4XeSS::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

    ImGui::TextUnformatted("RE4 XeSS");

    const auto producer = get_producer_snapshot();
    const auto handoff = m_output_handoff.snapshot();
    const auto requested_mode = m_requested_mode.load(std::memory_order_acquire);
    auto selected_mode = static_cast<int32_t>(requested_mode);
    if (ImGui::Combo("Upscaling Mode", &selected_mode, UPSCALING_MODE_LABELS.data(), static_cast<int32_t>(UPSCALING_MODE_LABELS.size()))) {
        request_mode(static_cast<UpscalingMode>(selected_mode));
        if (g_framework != nullptr) {
            g_framework->request_save_config();
        }
    }

    if (requested_mode == UpscalingMode::Off) {
        ImGui::TextUnformatted("Off - native RE4 rendering");
        if (handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::MissingMarker ||
            handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::Draining ||
            handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined) {
            ImGui::TextWrapped("Previous XeSS output handoff: %s",
                handoff.failure_reason.empty() ? "draining or quarantined" : handoff.failure_reason.c_str());
        }
        return;
    }

    if (producer.faulted) {
        ImGui::TextWrapped("XeSS execute unavailable: %s", producer.failure_reason.c_str());
        return;
    }
    if (producer.draining) {
        ImGui::TextUnformatted("XeSS execute draining for reconfiguration");
        return;
    }
    if (producer.execution_ready) {
        if (handoff.installed) {
            ImGui::TextUnformatted("XeSS output handoff active - RE4 presentation path");
        } else if (handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::MissingMarker ||
            handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::Draining ||
            handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::WriterPending) {
            ImGui::TextUnformatted("XeSS output handoff draining");
            if (!handoff.failure_reason.empty()) {
                ImGui::TextWrapped("%s", handoff.failure_reason.c_str());
            }
        } else if (handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined) {
            ImGui::TextWrapped("XeSS output handoff unavailable: %s",
                handoff.failure_reason.empty() ? "generation quarantined" : handoff.failure_reason.c_str());
        } else {
            ImGui::TextUnformatted("XeSS execute active - waiting for output handoff");
        }
        ImGui::Text("Display: %ux%u  Input: %ux%u  Generation: %llu",
            producer.display.x,
            producer.display.y,
            producer.input.optimal.x,
            producer.input.optimal.y,
            static_cast<unsigned long long>(m_temporal_generation));
        return;
    }
    if (m_temporal_ready) {
        ImGui::TextUnformatted("Temporal inputs active - waiting for XeSS execution initialization");
        ImGui::Text("Display: %ux%u  Input: %ux%u  Generation: %llu",
            m_display_resolution.x,
            m_display_resolution.y,
            m_input_resolution.optimal.x,
            m_input_resolution.optimal.y,
            static_cast<unsigned long long>(m_temporal_generation));
        return;
    }

    if (!m_temporal_failure_reason.empty()) {
        ImGui::TextWrapped("Temporal setup unavailable: %s", m_temporal_failure_reason.c_str());
        return;
    }

    if (!producer.failure_reason.empty()) {
        ImGui::TextWrapped("XeSS runtime unavailable: %s", producer.failure_reason.c_str());
    } else if (producer.context_ready) {
        ImGui::TextUnformatted("XeSS producer initialized; waiting for temporal inputs");
    } else {
        ImGui::TextUnformatted("Waiting for the RE4 XeSS worker and pre-Overlay request");
    }
}

void RE4XeSS::on_post_present() {
    if (!sdk::GameIdentity::get().is_re4() || g_framework == nullptr ||
        g_framework->get_renderer_type() != REFramework::RendererType::D3D12) {
        return;
    }
    const auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr) {
        return;
    }
    auto& lifetime_trace = RE4XeSSLifetimeTrace::instance();
    const auto debug_log = REFrameworkConfig::get() != nullptr &&
        REFrameworkConfig::get()->is_debug_log_enabled();
    lifetime_trace.configure(debug_log && sdk::GameIdentity::get().is_re4());
    hook->set_present_diagnostics_enabled(lifetime_trace.enabled());
    const auto present = hook->get_present_diagnostics_snapshot();
    const auto handoff = m_output_handoff.snapshot();
    const bool present_valid = present.active && present.returned && present.result_valid &&
        present.return_thread_id == GetCurrentThreadId();
    const bool marker_was_pending = handoff.marker_pending;
    const auto prior_signaled_value = handoff.last_signaled_fence_value;
    RE4XeSSLifetimeTrace::Event present_event{};
    present_event.kind = RE4XeSSLifetimeTrace::Kind::PostPresentCallback;
    present_event.trace_id = handoff.marker_pending ? handoff.installed_trace_id : 0;
    present_event.frame_id = handoff.marker_pending ? handoff.installed_frame : 0;
    present_event.frame_valid = handoff.marker_pending && handoff.installed_trace_id != 0;
    present_event.present_ordinal = present.ordinal;
    present_event.callback_ordinal = present.post_present_callback_ordinal;
    present_event.install_id = handoff.install_id;
    present_event.related_present_ordinal = handoff.marker_pending ? handoff.installed_present_ordinal : 0;
    present_event.output_generation = handoff.output_generation;
    present_event.control_generation = handoff.installed_control_generation;
    present_event.device_reset_generation = handoff.installed_device_reset_generation;
    present_event.downstream_fence_value = handoff.last_signaled_fence_value;
    present_event.output_resource = handoff.output_resource;
    present_event.target_state = handoff.target_state;
    present_event.overlay = handoff.overlay;
    present_event.swapchain = present.swapchain;
    present_event.device = present.device;
    present_event.queue = present.command_queue;
    present_event.command_queue_type = present.command_queue_type;
    present_event.command_queue_type_valid = present.command_queue_type_valid;
    present_event.thread_id = GetCurrentThreadId();
    present_event.present_entry_thread_id = present.entry_thread_id;
    present_event.present_return_thread_id = present.return_thread_id;
    present_event.present_source = static_cast<int32_t>(present.source);
    present_event.result = present.result_valid ? static_cast<int32_t>(present.result) : E_PENDING;
    present_event.present_valid = present_valid;
    present_event.present_returned = present.returned;
    present_event.present1 = present.present1;
    present_event.present_callbacks_suppressed = present.render_callbacks_suppressed;
    present_event.original_present_skipped = present.original_call_skipped;
    present_event.mapping_ambiguous = handoff.marker_pending;
    present_event.mapping_observed = handoff.marker_pending;
    present_event.mapping_state = handoff.marker_pending && present_valid
        ? RE4XeSSLifetimeTrace::MappingState::InferredCandidate
        : RE4XeSSLifetimeTrace::MappingState::Unknown;
    present_event.present_entry_time_us = present.entry_time_us;
    present_event.original_present_enter_time_us = present.original_call_enter_time_us;
    present_event.original_present_return_time_us = present.original_call_return_time_us;
    present_event.original_present_skipped = present.original_call_skipped;
    RE4XeSSLifetimeTrace::set_reason(present_event,
        !present.active ? "callback-present-context-inactive" :
        !present.returned ? "callback-before-present-return" :
        present.render_callbacks_suppressed ? "unexpected-suppressed-post-callback" :
        "post-present-callback-entered");
    lifetime_trace.record(present_event);

    const RE4XeSSOutputHandoff::ObservationContext observation{
        static_cast<int32_t>(m_requested_mode.load(std::memory_order_acquire)),
        m_control_generation.load(std::memory_order_acquire),
        m_device_reset_generation.load(std::memory_order_acquire),
        0,
        GetCurrentThreadId(),
        false,
        0,
        0,
        present_valid ? present.ordinal : 0,
        0,
        0,
        present.swapchain,
        present.device,
        present.command_queue,
        0,
        present_valid,
        present.command_queue_type,
        present.command_queue_type_valid,
    };

    m_output_handoff.on_post_present(hook->get_device(), hook->get_command_queue(), observation);
    const auto handoff_after_present = m_output_handoff.snapshot();
    if (marker_was_pending && present_valid && lifetime_trace.note_pending_present(
            handoff.install_id, present.ordinal)) {
        if (lifetime_trace.claim_anomaly_window(
                RE4XeSSLifetimeTrace::AnomalyWindow::MarkerPendingAcrossPresent)) {
            dump_re4_xess_lifetime_trace(
                "marker-pending-across-two-present-intervals",
                32,
                RE4XeSSLifetimeTrace::DumpWindow::Anomaly);
        }
    }
    if (marker_was_pending && !handoff_after_present.marker_pending &&
        handoff_after_present.last_signaled_fence_value > prior_signaled_value &&
        lifetime_trace.claim_first_marker_queued_window()) {
        dump_re4_xess_lifetime_trace(
            "first-downstream-marker-queued",
            32,
            RE4XeSSLifetimeTrace::DumpWindow::ActiveMilestone);
    }
    if (lifetime_trace.claim_first_mapping_window()) {
        dump_re4_xess_lifetime_trace(
            "first-present-mapping-observation-not-proof",
            32,
            RE4XeSSLifetimeTrace::DumpWindow::Anomaly);
    }
    if (handoff_after_present.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined &&
        !handoff_after_present.failure_reason.empty() &&
        lifetime_trace.claim_anomaly_window(RE4XeSSLifetimeTrace::AnomalyWindow::WriterOrFenceFailure)) {
        dump_re4_xess_lifetime_trace(
            "writer-or-fence-quarantine",
            32,
            RE4XeSSLifetimeTrace::DumpWindow::Anomaly);
    }
    if (lifetime_trace.enabled() && lifetime_trace.active_capture_started() &&
        present.ordinal != 0 && present.ordinal % 512 == 0) {
        dump_re4_xess_lifetime_trace(
            "present-window-checkpoint",
            32,
            RE4XeSSLifetimeTrace::DumpWindow::Periodic);
    }
}

void RE4XeSS::on_device_reset() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }
    m_device_reset_generation.fetch_add(1, std::memory_order_acq_rel);
}

void RE4XeSS::on_config_load(const utility::Config& cfg) {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

    m_handoff_provenance_opt_in = cfg.get<bool>(std::string{ HANDOFF_PROVENANCE_CONFIG_KEY }).value_or(false);
    const auto debug_log = REFrameworkConfig::get() != nullptr &&
        REFrameworkConfig::get()->is_debug_log_enabled();
    if (debug_log) {
        log_re4_xess_runtime_identity();
    }
    const bool d3d12_renderer = g_framework != nullptr &&
        g_framework->get_renderer_type() == REFramework::RendererType::D3D12;
    auto& lifetime_trace = RE4XeSSLifetimeTrace::instance();
    lifetime_trace.configure(debug_log && d3d12_renderer);
    if (d3d12_renderer) {
        const auto& hook = g_framework->get_d3d12_hook();
        if (hook != nullptr) {
            hook->set_present_diagnostics_enabled(lifetime_trace.enabled());
        }
    }
    const auto image = TargetStateFactoryProbe::instance().image_identity();
    const bool provenance_enabled = m_handoff_provenance_opt_in && debug_log && d3d12_renderer &&
        image.expected_re4_build;
    const auto provenance_reason = !m_handoff_provenance_opt_in ? "opt-in-disabled" :
        !debug_log ? "debug-log-disabled" :
        !d3d12_renderer ? "renderer-not-d3d12" :
        !image.valid ? "main-image-identity-unreadable" :
        !image.expected_re4_build ? "unsupported-re4-image" : "enabled";
    const bool engine_writer_witness_enabled = d3d12_renderer && image.expected_re4_build;
    m_output_handoff.configure_transition_provenance(
        provenance_enabled, provenance_reason, image.base, image.size, image.checksum);
    TargetStateFactoryProbe::instance().configure_overlay_writer_probe(
        engine_writer_witness_enabled, provenance_enabled);

    const auto persisted_mode = cfg.get<std::string>(std::string{ UPSCALING_MODE_CONFIG_KEY });
    if (!persisted_mode) {
        return;
    }

    const auto parsed_mode = mode_from_config_token(*persisted_mode);
    const auto next_mode = parsed_mode.value_or(UpscalingMode::Off);
    if (!parsed_mode && m_last_invalid_config_token != *persisted_mode) {
        spdlog::warn("[RE4XeSS][Config] Unknown Upscaling Mode token '{}'; falling back to Off", *persisted_mode);
        m_last_invalid_config_token = *persisted_mode;
    } else if (parsed_mode) {
        m_last_invalid_config_token.clear();
    }

    request_mode(next_mode);
}

void RE4XeSS::on_config_save(utility::Config& cfg) {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

    cfg.set<std::string>(
        std::string{ UPSCALING_MODE_CONFIG_KEY },
        std::string{ mode_to_config_token(m_requested_mode.load(std::memory_order_acquire)) });
    cfg.set<bool>(std::string{ HANDOFF_PROVENANCE_CONFIG_KEY }, m_handoff_provenance_opt_in);
}

void RE4XeSS::request_mode(UpscalingMode mode) {
    const auto old_mode = m_requested_mode.load(std::memory_order_acquire);
    if (mode == old_mode) {
        return;
    }

    m_requested_mode.store(mode, std::memory_order_release);
    const auto control_generation = m_control_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto request_thread_id = GetCurrentThreadId();
    m_output_handoff.note_mode_transition(
        control_generation,
        static_cast<int32_t>(mode),
        m_device_reset_generation.load(std::memory_order_acquire));
    TargetStateFactoryProbe::instance().note_overlay_writer_transition(
        control_generation,
        static_cast<int32_t>(mode),
        m_device_reset_generation.load(std::memory_order_acquire));
    auto& lifetime_trace = RE4XeSSLifetimeTrace::instance();
    const auto installed = m_output_handoff.snapshot();
    if (lifetime_trace.enabled() && installed.installed) {
        RE4XeSSLifetimeTrace::Event event{};
        event.kind = RE4XeSSLifetimeTrace::Kind::ModeTransition;
        event.install_id = installed.install_id;
        event.frame_id = installed.installed_frame;
        event.frame_valid = installed.installed_frame != 0;
        event.present_ordinal = installed.installed_present_ordinal;
        event.output_generation = installed.output_generation;
        event.control_generation = control_generation;
        event.device_reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
        event.output_resource = installed.output_resource;
        event.target_state = installed.target_state;
        event.overlay = installed.overlay;
        event.thread_id = request_thread_id;
        event.result = static_cast<int32_t>(old_mode);
        RE4XeSSLifetimeTrace::set_reason(event, "requested-mode-transition-with-installed-handoff");
        lifetime_trace.record(event);
        dump_re4_xess_lifetime_trace(
            "mode-transition-with-installed-handoff",
            32,
            RE4XeSSLifetimeTrace::DumpWindow::ModeTransition);
    }
    spdlog::info("[RE4XeSS][Config] mode changed: {} -> {} controlGeneration={} requestThread={}",
        mode_to_display_label(old_mode),
        mode_to_display_label(mode),
        static_cast<unsigned long long>(control_generation),
        request_thread_id);

    if (REFrameworkConfig::get()->is_debug_log_enabled()) {
        const auto quality = mode_to_quality_setting(mode);
        if (quality) {
            spdlog::info("[RE4XeSS][Config] selected public XeSS quality enum={}", static_cast<int32_t>(*quality));
        }
    }
}

void RE4XeSS::publish_producer_snapshot(ProducerSnapshot snapshot) {
    std::lock_guard lock{ m_producer_snapshot_mutex };
    m_producer_snapshot = std::move(snapshot);
}

void RE4XeSS::publish_worker_snapshot(const RE4XeSSWorker::Snapshot& snapshot) {
    ProducerSnapshot producer{};
    producer.context_ready = snapshot.context_ready;
    producer.execution_ready = snapshot.execution_ready;
    producer.draining = snapshot.draining;
    producer.faulted = snapshot.faulted;
    producer.mode = static_cast<UpscalingMode>(snapshot.mode_token);
    producer.quality = snapshot.quality;
    producer.display = snapshot.display;
    producer.input = snapshot.input;
    producer.device_identity = snapshot.device_identity;
    producer.queue_identity = snapshot.queue_identity;
    producer.bridge_idle = snapshot.bridge_idle;
    producer.bridge_quarantined = snapshot.bridge_quarantined;
    producer.bridge_device_removed = snapshot.bridge_device_removed;
    producer.control_generation = snapshot.control_generation;
    producer.device_reset_generation = snapshot.device_reset_generation;
    producer.failure_reason = snapshot.failure_reason;
    publish_producer_snapshot(std::move(producer));
}

RE4XeSS::ProducerSnapshot RE4XeSS::get_producer_snapshot() const {
    std::lock_guard lock{ m_producer_snapshot_mutex };
    return m_producer_snapshot;
}

void RE4XeSS::set_owner_unavailable(std::string reason, bool draining, bool faulted) {
    auto snapshot = get_producer_snapshot();
    snapshot.mode = m_requested_mode.load(std::memory_order_acquire);
    snapshot.context_ready = false;
    snapshot.execution_ready = false;
    snapshot.draining = draining;
    snapshot.faulted = faulted;
    snapshot.failure_reason = std::move(reason);
    publish_producer_snapshot(std::move(snapshot));
}

void RE4XeSS::mark_execution_fault(std::string reason) {
    {
        std::lock_guard lock{ m_pending_worker_fault_mutex };
        m_pending_worker_fault_reason = reason;
        m_pending_worker_fault_generation = m_control_generation.load(std::memory_order_acquire);
        m_pending_worker_fault_reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
    }
    spdlog::error("[RE4XeSS][Failure] {}", reason);
    set_owner_unavailable(std::move(reason), false, true);
}

std::string RE4XeSS::pending_worker_fault(uint64_t control_generation, uint64_t device_reset_generation) {
    std::lock_guard lock{ m_pending_worker_fault_mutex };
    if (m_pending_worker_fault_generation != control_generation ||
        m_pending_worker_fault_reset_generation != device_reset_generation) {
        m_pending_worker_fault_reason.clear();
        return {};
    }
    return m_pending_worker_fault_reason;
}

void RE4XeSS::acknowledge_worker_fault(uint64_t control_generation, uint64_t device_reset_generation) {
    std::lock_guard lock{ m_pending_worker_fault_mutex };
    if (m_pending_worker_fault_generation == control_generation &&
        m_pending_worker_fault_reset_generation == device_reset_generation) {
        m_pending_worker_fault_reason.clear();
    }
}
bool RE4XeSS::get_display_resolution(xess_2d_t& resolution) const {
    resolution = {};

    if (g_framework == nullptr || g_framework->get_renderer_type() != REFramework::RendererType::D3D12) {
        return false;
    }

    const auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr) {
        return false;
    }

    auto* swapchain = hook->get_swap_chain();
    if (swapchain == nullptr) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    if (FAILED(swapchain->GetDesc1(&description)) || description.Width == 0 || description.Height == 0) {
        return false;
    }

    resolution = { description.Width, description.Height };
    return true;
}

bool RE4XeSS::is_temporal_active() const {
    const auto requested_mode = m_requested_mode.load(std::memory_order_acquire);
    const auto producer = get_producer_snapshot();
    return sdk::GameIdentity::get().is_re4() &&
        requested_mode != UpscalingMode::Off &&
        requested_mode == m_temporal_mode &&
        producer.context_ready &&
        !producer.draining &&
        !producer.faulted &&
        producer.mode == requested_mode &&
        producer.control_generation == m_control_generation.load(std::memory_order_acquire) &&
        producer.device_reset_generation == m_device_reset_generation.load(std::memory_order_acquire) &&
        producer.display.x == m_display_resolution.x &&
        producer.display.y == m_display_resolution.y &&
        m_last_frame_device_reset_generation.load(std::memory_order_acquire) ==
            m_device_reset_generation.load(std::memory_order_acquire) &&
        m_temporal_ready;
}

void RE4XeSS::invalidate_history(std::string_view reason, bool reset_jitter) {
    const bool was_valid = !m_history_invalid || !m_first_valid_frame_reset_pending;
    m_history_invalid = true;
    m_first_valid_frame_reset_pending = true;
    m_scene_history = {};
    m_last_processed_scene_frame.reset();
    m_next_jitter_sample = 0;

    if (reason != m_last_reset_reason && REFrameworkConfig::get()->is_debug_log_enabled()) {
        spdlog::info("[RE4XeSS][Reset] reason={}", reason);
    }
    m_last_reset_reason = reason;

    if (reset_jitter) {
        m_next_jitter_sample = 0;
    }

    if (was_valid) {
        m_latest_frame_snapshot.reset();
    }
}

void RE4XeSS::clear_frame_state() {
    m_cached_scene = nullptr;
    m_cached_scene_frame.reset();
    m_cached_jitter_x = 0.0f;
    m_cached_jitter_y = 0.0f;
    m_cached_vertical_fov = 0.0f;
    m_camera_near = 0.0f;
    m_camera_far = 0.0f;
    m_camera_frame.reset();
    m_camera_metadata_valid = false;
}

void RE4XeSS::clear_resource_identities() {
    m_resource_identity_valid = false;
    m_last_color_identity = 0;
    m_last_depth_identity = 0;
    m_last_velocity_identity = 0;
}

void RE4XeSS::reset_temporal_state(std::string_view reason, bool reset_load_state) {
    m_temporal_ready = false;
    m_temporal_signature_valid = false;
    m_input_resolution_valid = false;
    m_temporal_failure_reason.clear();
    m_display_resolution = {};
    m_input_resolution = {};
    m_jitter_phase_count = 8;
    clear_resource_identities();
    m_latest_frame_snapshot.reset();
    m_last_scene_callback_frame.reset();
    clear_frame_state();
    invalidate_history(reason);

    if (!reset_load_state) {
        return;
    }

    LoadStateUpdateScope load_state_update{ *this };
    m_pause_previous_valid = false;
    m_pause_previous = false;
    m_load_transition_active = false;
    m_inhibit_departure_pending = false;
    m_remembered_normal_inhibit_valid = false;
    m_remembered_normal_inhibit = 0;
    m_departure_inhibit = 0;
    m_startup_mid_load = false;
    m_post_pause_rebaseline_candidate_valid = false;
    m_post_pause_rebaseline_transition_seen = false;
    m_post_pause_rebaseline_candidate = 0;
    m_post_pause_rebaseline_stable_count = 0;
    m_load_observation_valid = false;
}

void RE4XeSS::update_temporal_configuration() {
    const auto previous_ready = m_temporal_ready;
    const auto requested_mode = m_requested_mode.load(std::memory_order_acquire);
    const auto producer = get_producer_snapshot();
    auto set_unavailable = [&](std::string reason, std::string_view reset_reason) {
        if (m_temporal_ready) {
            invalidate_history(reset_reason);
            clear_resource_identities();
            m_temporal_signature_valid = false;
            m_input_resolution_valid = false;
        }
        m_temporal_ready = false;
        if (m_temporal_failure_reason != reason && REFrameworkConfig::get()->is_debug_log_enabled()) {
            spdlog::warn("[RE4XeSS][Temporal] {}", reason);
        }
        m_temporal_failure_reason = std::move(reason);
    };

    if (!sdk::GameIdentity::get().is_re4() || requested_mode == UpscalingMode::Off) {
        set_unavailable("Off - native RE4 rendering", "mode-off");
        return;
    }

    if (producer.control_generation != m_control_generation.load(std::memory_order_acquire) ||
        producer.device_reset_generation != m_device_reset_generation.load(std::memory_order_acquire)) {
        set_unavailable("Waiting for the worker to accept the current control/device generation",
            "producer-generation-stale");
        m_temporal_signature_valid = false;
        m_input_resolution_valid = false;
        return;
    }

    if (producer.faulted) {
        set_unavailable(producer.failure_reason, "producer-faulted");
        m_temporal_signature_valid = false;
        m_input_resolution_valid = false;
        return;
    }

    if (!producer.context_ready || producer.draining || producer.mode != requested_mode) {
        set_unavailable(producer.draining
                ? "XeSS bridge is draining for reconfiguration"
                : "Waiting for the pre-Overlay XeSS producer query",
            "producer-context-unavailable");
        m_temporal_signature_valid = false;
        m_input_resolution_valid = false;
        return;
    }

    const auto quality = mode_to_quality_setting(requested_mode);
    if (!quality) {
        set_unavailable("The requested RE4 upscaling mode has no XeSS quality mapping", "invalid-quality-mode");
        return;
    }

    xess_2d_t display{};
    if (!get_display_resolution(display)) {
        set_unavailable("Waiting for a valid nonzero D3D12 swapchain display extent", "display-extent-unavailable");
        m_temporal_signature_valid = false;
        m_input_resolution_valid = false;
        return;
    }

    if (producer.display.x != display.x || producer.display.y != display.y || producer.quality != *quality) {
        set_unavailable("Waiting for the pre-Overlay owner to query the current display/quality", "producer-signature-stale");
        m_temporal_signature_valid = false;
        m_input_resolution_valid = false;
        return;
    }

    const bool configuration_changed =
        !m_temporal_signature_valid ||
        m_temporal_mode != requested_mode ||
        m_temporal_quality != *quality ||
        m_display_resolution.x != display.x ||
        m_display_resolution.y != display.y;

    if (configuration_changed) {
        if (m_temporal_signature_valid || m_temporal_generation == 0) {
            invalidate_history("temporal-configuration-change");
        }
        ++m_temporal_generation;
        m_temporal_mode = requested_mode;
        m_temporal_quality = *quality;
        m_display_resolution = display;
        m_temporal_signature_valid = true;
        m_temporal_ready = false;
        m_input_resolution_valid = false;
        clear_resource_identities();
    }

    const auto& query = producer.input;

    const auto render = query.optimal;
    if (render.x > display.x || render.y > display.y) {
        set_unavailable(
            "XeSS frontend optimal input extent exceeds the display extent",
            "input-extent-exceeds-display");
        return;
    }

    const uint64_t render_display_cross = static_cast<uint64_t>(render.x) * display.y;
    const uint64_t display_render_cross = static_cast<uint64_t>(render.y) * display.x;
    const uint64_t cross_error = render_display_cross > display_render_cross
        ? render_display_cross - display_render_cross
        : display_render_cross - render_display_cross;
    const auto aspect_denominator = static_cast<double>(render.y) * display.x;
    const auto relative_aspect_error = aspect_denominator > 0.0
        ? static_cast<double>(cross_error) / aspect_denominator
        : std::numeric_limits<double>::infinity();

    if (!std::isfinite(relative_aspect_error) || relative_aspect_error > 0.01) {
        set_unavailable(
            "XeSS frontend aspect mismatch: display=" + std::to_string(display.x) + "x" + std::to_string(display.y) +
                " input=" + std::to_string(render.x) + "x" + std::to_string(render.y) +
                " relativeError=" + std::to_string(relative_aspect_error),
            "input-aspect-mismatch");
        return;
    }

    const bool query_changed = !m_input_resolution_valid ||
        m_input_resolution.optimal.x != query.optimal.x ||
        m_input_resolution.optimal.y != query.optimal.y ||
        m_input_resolution.minimum.x != query.minimum.x ||
        m_input_resolution.minimum.y != query.minimum.y ||
        m_input_resolution.maximum.x != query.maximum.x ||
        m_input_resolution.maximum.y != query.maximum.y;

    if (query_changed) {
        if (!configuration_changed) {
            ++m_temporal_generation;
            invalidate_history("frontend-input-resolution-change");
            clear_resource_identities();
        }
        m_input_resolution = query;
        m_input_resolution_valid = true;
    } else if (!previous_ready) {
        ++m_temporal_generation;
        invalidate_history("temporal-setup-recovered");
    }

    const auto scale_x = static_cast<double>(display.x) / render.x;
    const auto scale_y = static_cast<double>(display.y) / render.y;
    const auto scale = std::max(scale_x, scale_y);
    const auto requested_phase_count = std::max(8.0, std::ceil(8.0 * scale * scale));
    m_jitter_phase_count = static_cast<uint32_t>(std::min(
        requested_phase_count,
        static_cast<double>(std::numeric_limits<uint32_t>::max())));
    m_temporal_ready = true;
    m_temporal_failure_reason.clear();

    if ((!previous_ready || configuration_changed || query_changed) && REFrameworkConfig::get()->is_debug_log_enabled()) {
        spdlog::info(
            "[RE4XeSS][Temporal] mode={} display={}x{} input={}x{} min={}x{} max={}x{} phases={} generation={}",
            mode_to_display_label(requested_mode),
            display.x,
            display.y,
            query.optimal.x,
            query.optimal.y,
            query.minimum.x,
            query.minimum.y,
            query.maximum.x,
            query.maximum.y,
            m_jitter_phase_count,
            static_cast<unsigned long long>(m_temporal_generation));
    }
}

void RE4XeSS::update_load_state() {
    LoadStateUpdateScope load_state_update{ *this };
    const auto observation = read_game_load_snapshot();
    log_load_accessor_observation(observation);
    if (!observation.snapshot) {
        m_load_observation_valid = false;
        invalidate_history("load-state-observation-unavailable");
        return;
    }
    const auto& snapshot = *observation.snapshot;

    const auto log_load_event = [](std::string_view message) {
        if (REFrameworkConfig::get()->is_debug_log_enabled()) {
            spdlog::info("[RE4XeSS][Reset] {}", message);
        }
    };

    m_load_observation_valid = true;

    if (!m_pause_previous_valid) {
        m_pause_previous_valid = true;
        m_pause_previous = snapshot.pause;
        if (snapshot.pause) {
            m_startup_mid_load = true;
            m_load_transition_active = true;
            m_remembered_normal_inhibit_valid = false;
            m_post_pause_rebaseline_candidate_valid = false;
            m_post_pause_rebaseline_transition_seen = false;
            m_post_pause_rebaseline_stable_count = 0;
            invalidate_history("startup-observed-mid-load");
            log_load_event("load pause observed at startup; no pre-load baseline assumed");
        } else if (!m_remembered_normal_inhibit_valid) {
            m_remembered_normal_inhibit = snapshot.inhibit;
            m_remembered_normal_inhibit_valid = true;
        }
        return;
    }

    if (snapshot.pause) {
        if (!m_pause_previous) {
            const bool witnessed_pre_pause_departure =
                m_inhibit_departure_pending && m_remembered_normal_inhibit_valid;
            if (m_remembered_normal_inhibit_valid && snapshot.inhibit != m_remembered_normal_inhibit) {
                m_departure_inhibit = snapshot.inhibit;
            }

            m_load_transition_active = true;
            m_inhibit_departure_pending = false;
            if (!witnessed_pre_pause_departure) {
                m_startup_mid_load = true;
                m_remembered_normal_inhibit_valid = false;
                m_post_pause_rebaseline_candidate_valid = false;
                m_post_pause_rebaseline_transition_seen = false;
                m_post_pause_rebaseline_stable_count = 0;
                log_load_event("load pause entered without a witnessed pre-pause departure; treating the remembered baseline as untrusted");
            } else if (REFrameworkConfig::get()->is_debug_log_enabled()) {
                spdlog::info("[RE4XeSS][Reset] load pause confirmed; keeping frozen normal InhibitBit={:#x}",
                    static_cast<unsigned long long>(m_remembered_normal_inhibit));
            }
            m_post_pause_rebaseline_candidate_valid = false;
            m_post_pause_rebaseline_transition_seen = false;
            m_post_pause_rebaseline_stable_count = 0;
            invalidate_history("load-pause-entered");
        }
        m_pause_previous = true;
        return;
    }

    const bool pause_just_released = m_pause_previous;
    m_pause_previous = false;

    if (m_startup_mid_load) {
        if (pause_just_released) {
            m_post_pause_rebaseline_candidate = snapshot.inhibit;
            m_post_pause_rebaseline_candidate_valid = true;
            m_post_pause_rebaseline_transition_seen = false;
            m_post_pause_rebaseline_stable_count = 0;
            log_load_event("load pause released; keeping history blocked until a post-pause InhibitBit transition and stable rebaseline");
            return;
        }

        if (!m_post_pause_rebaseline_candidate_valid) {
            m_post_pause_rebaseline_candidate = snapshot.inhibit;
            m_post_pause_rebaseline_candidate_valid = true;
            m_post_pause_rebaseline_transition_seen = false;
            m_post_pause_rebaseline_stable_count = 0;
            return;
        }

        if (!m_post_pause_rebaseline_transition_seen) {
            if (snapshot.inhibit == m_post_pause_rebaseline_candidate) {
                return;
            }

            const auto previous_candidate = m_post_pause_rebaseline_candidate;
            m_post_pause_rebaseline_candidate = snapshot.inhibit;
            m_post_pause_rebaseline_transition_seen = true;
            m_post_pause_rebaseline_stable_count = 1;
            if (REFrameworkConfig::get()->is_debug_log_enabled()) {
                spdlog::info("[RE4XeSS][Reset] post-pause InhibitBit transition {:#x}->{:#x}; beginning stable rebaseline observations",
                    static_cast<unsigned long long>(previous_candidate),
                    static_cast<unsigned long long>(m_post_pause_rebaseline_candidate));
            }
            return;
        }

        if (snapshot.inhibit != m_post_pause_rebaseline_candidate) {
            m_post_pause_rebaseline_candidate = snapshot.inhibit;
            m_post_pause_rebaseline_stable_count = 1;
            return;
        }

        if (m_post_pause_rebaseline_stable_count < 3) {
            ++m_post_pause_rebaseline_stable_count;
        }
        if (m_post_pause_rebaseline_stable_count >= 3) {
            m_remembered_normal_inhibit = m_post_pause_rebaseline_candidate;
            m_remembered_normal_inhibit_valid = true;
            m_startup_mid_load = false;
            m_load_transition_active = false;
            m_inhibit_departure_pending = false;
            m_post_pause_rebaseline_candidate_valid = false;
            m_post_pause_rebaseline_transition_seen = false;
            m_post_pause_rebaseline_stable_count = 0;
            m_first_valid_frame_reset_pending = true;
            invalidate_history("startup-load-rebaseline-complete");
            if (REFrameworkConfig::get()->is_debug_log_enabled()) {
                spdlog::info("[RE4XeSS][Reset] adopted post-pause InhibitBit baseline={:#x} after three stable observations",
                    static_cast<unsigned long long>(m_remembered_normal_inhibit));
            }
        }
        return;
    }

    if (m_load_transition_active) {
        if (pause_just_released) {
            log_load_event("load pause released; keeping temporal history blocked until frozen normal InhibitBit returns");
        }
        if (!m_remembered_normal_inhibit_valid) {
            return;
        }

        if (snapshot.inhibit != m_remembered_normal_inhibit) {
            m_departure_inhibit = snapshot.inhibit;
            return;
        }

        m_load_transition_active = false;
        m_inhibit_departure_pending = false;
        m_post_pause_rebaseline_candidate_valid = false;
        m_post_pause_rebaseline_transition_seen = false;
        m_post_pause_rebaseline_stable_count = 0;
        m_first_valid_frame_reset_pending = true;
        invalidate_history("load-recovery-complete");
        log_load_event("load recovery completed after InhibitBit returned to frozen baseline");
        return;
    }

    if (!m_remembered_normal_inhibit_valid) {
        m_remembered_normal_inhibit = snapshot.inhibit;
        m_remembered_normal_inhibit_valid = true;
        return;
    }

    if (snapshot.inhibit != m_remembered_normal_inhibit) {
        if (!m_inhibit_departure_pending) {
            m_inhibit_departure_pending = true;
            m_departure_inhibit = snapshot.inhibit;
            invalidate_history("pre-pause-inhibit-departure");
            if (REFrameworkConfig::get()->is_debug_log_enabled()) {
                spdlog::info("[RE4XeSS][Reset] frozen normal InhibitBit={:#x} departed to {:#x}; keeping history blocked until Pause or return",
                    static_cast<unsigned long long>(m_remembered_normal_inhibit),
                    static_cast<unsigned long long>(m_departure_inhibit));
            }
        }
        return;
    }

    if (m_inhibit_departure_pending) {
        m_inhibit_departure_pending = false;
        m_departure_inhibit = 0;
        invalidate_history("pre-pause-inhibit-return");
        log_load_event("InhibitBit returned before Pause; suspicion cleared with history reset retained");
    }
}

void RE4XeSS::on_view_get_size(REManagedObject* scene_view, float* result) {
    auto load_window = begin_load_state_trace();
    finish_load_state_trace(load_window);
    const bool temporal_active = is_temporal_active();
    // A prepared XeSS context does not authorize reduced SceneView rendering
    // while the already-observed LoadAccessor state forbids a new XeSS frame.
    const bool load_admitted = RE4XeSSLoadEligibility::allows_temporal_rendering(load_window.state);
    const bool load_observation_valid = load_window.state.observation_valid;
    trace_load_state_admission(0, load_observation_valid
            ? (load_admitted ? "eligible" : "native-load-window")
            : "observation-unavailable",
        load_window);
    const float native_width = result != nullptr ? result[0] : 0.0f;
    const float native_height = result != nullptr ? result[1] : 0.0f;
    const auto size_decision = RE4XeSSSceneView::decide_size(
        result != nullptr,
        native_width,
        native_height,
        temporal_active && load_admitted,
        load_observation_valid,
        m_input_resolution.optimal.x,
        m_input_resolution.optimal.y);
    const bool override_applied = size_decision.override_applied;

    if (override_applied) {
        result[0] = size_decision.effective_width;
        result[1] = size_decision.effective_height;
    }

    auto& lifetime_trace = RE4XeSSLifetimeTrace::instance();
    if (!lifetime_trace.enabled() || result == nullptr) {
        return;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame) {
        return;
    }

    RE4XeSSLifetimeTrace::Event trace_event{};
    trace_event.kind = RE4XeSSLifetimeTrace::Kind::SceneViewSize;
    trace_event.frame_id = static_cast<uint64_t>(*frame);
    trace_event.frame_valid = true;
    trace_event.scene_view = reinterpret_cast<uintptr_t>(scene_view);
    trace_event.scene_view_native_width = native_width;
    trace_event.scene_view_native_height = native_height;
    trace_event.scene_view_effective_width = result[0];
    trace_event.scene_view_effective_height = result[1];
    trace_event.input_width = m_input_resolution.optimal.x;
    trace_event.input_height = m_input_resolution.optimal.y;
    trace_event.display_width = m_display_resolution.x;
    trace_event.display_height = m_display_resolution.y;
    trace_event.input_resolution_valid = m_input_resolution_valid;
    trace_event.temporal_active = temporal_active;
    trace_event.scene_view_override_applied = override_applied;
    trace_event.load_state_callback_kind = 0;
    set_load_state_trace_fields(trace_event, load_window);
    trace_event.control_generation = m_control_generation.load(std::memory_order_acquire);
    trace_event.device_reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
    trace_event.thread_id = GetCurrentThreadId();
    RE4XeSSLifetimeTrace::set_reason(
        trace_event,
        override_applied ? "reduced-scene-view-applied" :
            temporal_active && !load_admitted ? "native-load-window" :
            temporal_active ? "temporal-view-gate-closed" : "native-scene-view");
    lifetime_trace.record(trace_event);
}

void RE4XeSS::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    auto load_window = begin_load_state_trace();
    finish_load_state_trace(load_window);
    const bool load_admitted = RE4XeSSLoadEligibility::allows_temporal_rendering(load_window.state);
    const bool temporal_active = is_temporal_active();
    trace_load_state_admission(1, !load_window.state.observation_valid
            ? "observation-unavailable"
            : load_admitted ? "eligible" : "native-load-window",
        load_window);
    if (!temporal_active || !load_admitted || camera == nullptr || result == nullptr) {
        return;
    }

    const auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || camera != reinterpret_cast<REManagedObject*>(const_cast<RECamera*>(primary_camera))) {
        return;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame) {
        return;
    }

    static sdk::RETypeDefinition* camera_type{};
    static sdk::REMethodDefinition* get_near_clip_plane{};
    static sdk::REMethodDefinition* get_far_clip_plane{};
    if (camera_type == nullptr) {
        camera_type = sdk::find_type_definition("via.Camera");
    }
    if (camera_type != nullptr) {
        if (get_near_clip_plane == nullptr) get_near_clip_plane = camera_type->get_method("get_NearClipPlane");
        if (get_far_clip_plane == nullptr) get_far_clip_plane = camera_type->get_method("get_FarClipPlane");
    }

    m_camera_frame = *frame;
    m_camera_metadata_valid = false;
    if (get_near_clip_plane == nullptr || get_far_clip_plane == nullptr) {
        return;
    }

    const auto near_plane = get_near_clip_plane->call_safe<float>(sdk::get_thread_context(), camera);
    const auto far_plane = get_far_clip_plane->call_safe<float>(sdk::get_thread_context(), camera);
    if (!std::isfinite(near_plane) || !std::isfinite(far_plane) || near_plane <= 0.0f || far_plane <= near_plane) {
        return;
    }

    m_camera_near = near_plane;
    m_camera_far = far_plane;
    m_camera_metadata_valid = true;
}

void RE4XeSS::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;
    if (!is_temporal_active() || layer == nullptr || !layer->is_fully_rendered()) {
        return;
    }

    const auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || layer->get_camera() != primary_camera) {
        return;
    }

    auto load_window = begin_load_state_trace();
    finish_load_state_trace(load_window);
    const bool load_admitted = RE4XeSSLoadEligibility::allows_temporal_rendering(load_window.state);
    trace_load_state_admission(2, !load_window.state.observation_valid
            ? "observation-unavailable"
            : load_admitted ? "eligible" : "native-load-window",
        load_window);
    if (!load_window.state.observation_valid) {
        invalidate_history("load-state-observation-unavailable");
        return;
    }
    if (!load_admitted) {
        invalidate_history("load-history-invalid");
        return;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame) {
        invalidate_history("renderer-frame-id-unavailable");
        return;
    }

    const auto frame_id = static_cast<uint64_t>(*frame);
    if (m_last_scene_callback_frame && *m_last_scene_callback_frame == frame_id) {
        return;
    }
    m_last_scene_callback_frame = frame_id;

    const bool frame_gap = m_last_processed_scene_frame &&
        frame_id != (*m_last_processed_scene_frame + 1);
    if (frame_gap) {
        invalidate_history("primary-scene-frame-gap");
    }
    m_last_processed_scene_frame = frame_id;

    if (!m_camera_frame || *m_camera_frame != frame_id || !m_camera_metadata_valid) {
        invalidate_history("same-frame-camera-metadata-unavailable");
        return;
    }

    auto infos = scene_infos(layer);
    if (infos[0] == nullptr) {
        invalidate_history("primary-scene-info-unavailable");
        return;
    }

    const auto projection_y = infos[0]->projection_matrix[1][1];
    if (!std::isfinite(projection_y) || std::abs(projection_y) < 0.000001f) {
        invalidate_history("primary-scene-fov-invalid");
        return;
    }
    const auto vertical_fov = 2.0f * std::atan(1.0f / projection_y);
    if (!std::isfinite(vertical_fov) || vertical_fov <= 0.0f) {
        invalidate_history("primary-scene-fov-invalid");
        return;
    }

    const bool variant_frame_gap = [&] {
        for (size_t i = 0; i < infos.size(); ++i) {
            if (infos[i] != nullptr && m_scene_history[i].valid &&
                frame_id != m_scene_history[i].frame_id + 1) {
                return true;
            }
        }
        return false;
    }();
    if (variant_frame_gap) {
        invalidate_history("scene-info-history-gap");
    }

    const auto sample_index = m_next_jitter_sample + 1;
    const auto jitter_x = halton(sample_index, 2) - 0.5f;
    const auto jitter_y = halton(sample_index, 3) - 0.5f;
    const auto matrix_jitter_x = 2.0f * jitter_x / static_cast<float>(m_input_resolution.optimal.x);
    const auto matrix_jitter_y = -2.0f * jitter_y / static_cast<float>(m_input_resolution.optimal.y);

    for (size_t i = 0; i < infos.size(); ++i) {
        auto* info = infos[i];
        if (info == nullptr) {
            continue;
        }

        const auto current_projection = info->projection_matrix;
        const auto current_view = info->view_matrix;
        auto& history = m_scene_history[i];
        const bool history_valid = history.valid && frame_id == history.frame_id + 1;
        auto previous_projection = history_valid ? history.unjittered_projection : current_projection;
        const auto previous_view = history_valid ? history.view : current_view;

        previous_projection[2][0] += matrix_jitter_x;
        previous_projection[2][1] += matrix_jitter_y;
        info->old_view_projection_matrix = previous_projection * previous_view;

        history.unjittered_projection = current_projection;
        history.view = current_view;
        history.frame_id = frame_id;
        history.valid = true;

        info->projection_matrix[2][0] += matrix_jitter_x;
        info->projection_matrix[2][1] += matrix_jitter_y;
        info->inverse_projection_matrix = glm::inverse(info->projection_matrix);
        info->view_projection_matrix = info->projection_matrix * info->view_matrix;
        info->inverse_view_projection_matrix = glm::inverse(info->view_projection_matrix);
    }

    m_next_jitter_sample = (m_next_jitter_sample + 1) % m_jitter_phase_count;
    m_cached_scene = layer;
    m_cached_scene_frame = frame_id;
    m_cached_jitter_x = jitter_x;
    m_cached_jitter_y = jitter_y;
    m_cached_vertical_fov = vertical_fov;

    auto& lifetime_trace = RE4XeSSLifetimeTrace::instance();
    if (lifetime_trace.enabled()) {
        RE4XeSSLifetimeTrace::Event trace_event{};
        trace_event.kind = RE4XeSSLifetimeTrace::Kind::SceneFrame;
        trace_event.frame_id = frame_id;
        trace_event.frame_valid = true;
        trace_event.scene_ordinal = m_scene_lifetime_ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
        trace_event.control_generation = m_control_generation.load(std::memory_order_acquire);
        trace_event.device_reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
        trace_event.input_width = m_input_resolution.optimal.x;
        trace_event.input_height = m_input_resolution.optimal.y;
        trace_event.display_width = m_display_resolution.x;
        trace_event.display_height = m_display_resolution.y;
        trace_event.input_resolution_valid = m_input_resolution_valid;
        trace_event.jitter_applied = true;
        trace_event.jitter_x_pixels = jitter_x;
        trace_event.jitter_y_pixels = jitter_y;
        trace_event.jitter_sample_index = static_cast<uint32_t>(sample_index);
        trace_event.jitter_phase_count = m_jitter_phase_count;
        trace_event.thread_id = GetCurrentThreadId();
        trace_event.load_state_callback_kind = 2;
        set_load_state_trace_fields(trace_event, load_window);
        RE4XeSSLifetimeTrace::set_reason(trace_event, "primary-scene-jitter-applied");
        lifetime_trace.record(trace_event);
    }
}

bool RE4XeSS::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    const auto callback_thread_id = GetCurrentThreadId();
    const auto debug_log = REFrameworkConfig::get()->is_debug_log_enabled();
    auto overlap_epoch = m_pre_overlay_overlap_epoch.load(std::memory_order_acquire);
    auto& lifetime_trace = RE4XeSSLifetimeTrace::instance();
    const bool d3d12_renderer = g_framework != nullptr &&
        g_framework->get_renderer_type() == REFramework::RendererType::D3D12;
    lifetime_trace.configure(debug_log && sdk::GameIdentity::get().is_re4() && d3d12_renderer);
    if (g_framework != nullptr &&
        g_framework->get_renderer_type() == REFramework::RendererType::D3D12) {
        const auto& hook = g_framework->get_d3d12_hook();
        if (hook != nullptr) {
            hook->set_present_diagnostics_enabled(lifetime_trace.enabled());
        }
    }
    const bool trace_enabled = lifetime_trace.enabled();
    auto load_window = begin_load_state_trace();
    finish_load_state_trace(load_window);
    const auto trace_id = trace_enabled ? lifetime_trace.next_trace_id() : 0;
    const auto callback_ordinal = trace_enabled
        ? m_pre_overlay_lifetime_ordinal.fetch_add(1, std::memory_order_relaxed) + 1
        : 0;
    uint64_t trace_frame_id{};
    bool trace_frame_valid{};
    D3D12Hook::PresentDiagnosticsSnapshot present_diagnostics{};
    const auto capture_present_diagnostics = [&] {
        present_diagnostics = {};
        if (g_framework == nullptr || g_framework->get_renderer_type() != REFramework::RendererType::D3D12) {
            return;
        }
        const auto& hook = g_framework->get_d3d12_hook();
        if (hook != nullptr) {
            present_diagnostics = hook->get_present_diagnostics_snapshot();
        }
    };
    capture_present_diagnostics();
    const auto present_is_current = [&] {
        return present_diagnostics.active &&
            present_diagnostics.ordinal != 0 &&
            present_diagnostics.entry_thread_id == callback_thread_id;
    };
    const auto record_lifetime_event = [&](
        RE4XeSSLifetimeTrace::Kind kind,
        std::string_view reason,
        uint64_t event_trace_id = 0) {
        if (!trace_enabled) {
            return uint64_t{};
        }
        RE4XeSSLifetimeTrace::Event event{};
        event.kind = kind;
        event.trace_id = event_trace_id != 0 ? event_trace_id : trace_id;
        event.frame_id = trace_frame_id;
        event.frame_valid = trace_frame_valid;
        event.callback_ordinal = callback_ordinal;
        event.overlap_epoch = overlap_epoch;
        event.present_ordinal = present_is_current() ? present_diagnostics.ordinal : 0;
        event.control_generation = m_control_generation.load(std::memory_order_acquire);
        event.device_reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
        event.swapchain = present_diagnostics.swapchain;
        event.device = present_diagnostics.device;
        event.queue = present_diagnostics.command_queue;
        event.command_queue_type = present_diagnostics.command_queue_type;
        event.command_queue_type_valid = present_diagnostics.command_queue_type_valid;
        event.thread_id = callback_thread_id;
        event.present_entry_thread_id = present_diagnostics.entry_thread_id;
        event.present_return_thread_id = present_diagnostics.return_thread_id;
        event.present_source = static_cast<int32_t>(present_diagnostics.source);
        event.present1 = present_diagnostics.present1;
        event.present_returned = present_diagnostics.returned;
        event.present_callbacks_suppressed = present_diagnostics.render_callbacks_suppressed;
        event.original_present_skipped = present_diagnostics.original_call_skipped;
        event.result = present_diagnostics.result_valid
            ? static_cast<int32_t>(present_diagnostics.result)
            : E_PENDING;
        event.present_valid = present_is_current();
        event.mapping_ambiguous = !present_is_current();
        event.load_state_callback_kind = 3;
        set_load_state_trace_fields(event, load_window);
        RE4XeSSLifetimeTrace::set_reason(event, reason);
        return lifetime_trace.record(event);
    };
    const auto trace_skip = [&](std::string_view reason) {
        if (!trace_enabled) {
            return;
        }
        (void)record_lifetime_event(RE4XeSSLifetimeTrace::Kind::Skip, reason);
        const auto skip_count = m_lifetime_skip_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!lifetime_trace.active_capture_started()) {
            if (skip_count == 1 || skip_count == 512) {
                dump_re4_xess_lifetime_trace(
                    reason,
                    32,
                    RE4XeSSLifetimeTrace::DumpWindow::PreActive);
            }
        } else if (reason == "output-handoff-restore-failed" ||
            reason == "output-handoff-install-failed" ||
            reason == "output-handoff-unavailable" ||
            reason == "xess-execute-fault") {
            if (lifetime_trace.claim_anomaly_window(
                    RE4XeSSLifetimeTrace::AnomalyWindow::WriterOrFenceFailure)) {
                dump_re4_xess_lifetime_trace(
                    reason,
                    32,
                    RE4XeSSLifetimeTrace::DumpWindow::Anomaly);
            }
        } else if (skip_count % 512 == 0) {
            dump_re4_xess_lifetime_trace(
                reason,
                32,
                RE4XeSSLifetimeTrace::DumpWindow::Periodic);
        }
    };

    if (m_pre_overlay_in_progress.test_and_set(std::memory_order_acquire)) {
        m_pre_overlay_overlap_epoch.fetch_add(1, std::memory_order_acq_rel);
        overlap_epoch = m_pre_overlay_overlap_epoch.load(std::memory_order_acquire);
        const auto overlap_count = m_pre_overlay_overlap_count.fetch_add(1, std::memory_order_relaxed) + 1;
        trace_skip("overlapping-pre-overlay-rejected");
        if (debug_log && overlap_count <= 8) {
            spdlog::warn("[RE4XeSS][Coordinator] overlapping pre-Overlay callback rejected thread={} count={}",
                callback_thread_id, static_cast<unsigned long long>(overlap_count));
        }
        return true;
    }

    const auto coordinator_log_index = m_pre_overlay_coordinator_log_count.fetch_add(1, std::memory_order_relaxed);
    const auto previous_thread_id = m_last_pre_overlay_thread_id.exchange(callback_thread_id, std::memory_order_acq_rel);
    const bool log_coordinator = debug_log && coordinator_log_index < 32;
    const bool thread_migrated = previous_thread_id != 0 && previous_thread_id != callback_thread_id;
    const auto migration_log_index = thread_migrated
        ? m_pre_overlay_migration_log_count.fetch_add(1, std::memory_order_relaxed)
        : 32;
    const bool log_migration = debug_log && thread_migrated && migration_log_index < 32;
    PreOverlayGateGuard gate_guard{
        m_pre_overlay_in_progress,
        log_coordinator,
        callback_thread_id,
        overlap_epoch,
    };
    trace_frame_valid = m_cached_scene_frame.has_value();
    trace_frame_id = m_cached_scene_frame.value_or(0);
    (void)record_lifetime_event(RE4XeSSLifetimeTrace::Kind::PreOverlay, "callback-enter");
    m_output_handoff.observe_fence_completion();
    TargetStateFactoryProbe::instance().observe_overlay_writer(layer);
    const auto engine_writer_witness_chain =
        TargetStateFactoryProbe::instance().confirmed_overlay_writer_witness_chain();
    if (m_output_handoff.identity_mismatch_latched()) {
        const auto producer = get_producer_snapshot();
        (void)m_output_handoff.poll_retirement(producer.bridge_idle, producer.bridge_device_removed);
        trace_skip("output-handoff-identity-mismatch-latched");
        return true;
    }
    if (log_coordinator) {
        spdlog::info("[RE4XeSS][Coordinator] callbackThread={} workerThread={} overlapEpoch={}",
            callback_thread_id,
            m_worker.thread_id(),
            static_cast<unsigned long long>(overlap_epoch));
    }
    if (log_migration) {
        spdlog::info("[RE4XeSS][Coordinator] callback thread migration {} -> {}; workerThread={} (informational)",
            previous_thread_id, callback_thread_id, m_worker.thread_id());
    }

    const auto requested_mode = m_requested_mode.load(std::memory_order_acquire);
    const auto control_generation = m_control_generation.load(std::memory_order_acquire);
    const auto reset_generation = m_device_reset_generation.load(std::memory_order_acquire);
    const auto cached_scene_frame = m_cached_scene_frame;
    const RE4XeSSOutputHandoff::ObservationContext handoff_observation{
        static_cast<int32_t>(requested_mode),
        control_generation,
        reset_generation,
        cached_scene_frame.value_or(0),
        callback_thread_id,
        cached_scene_frame.has_value(),
        trace_id,
        callback_ordinal,
        present_is_current() ? present_diagnostics.ordinal : 0,
        0,
        0,
        present_diagnostics.swapchain,
        present_diagnostics.device,
        present_diagnostics.command_queue,
        0,
        present_is_current(),
        present_diagnostics.command_queue_type,
        present_diagnostics.command_queue_type_valid,
    };

    std::string restore_error;
    uint64_t consumed_writer_terminal_sequence{};
    if (!m_output_handoff.restore(
            layer, handoff_observation, engine_writer_witness_chain, restore_error,
            consumed_writer_terminal_sequence)) {
        const auto handoff = m_output_handoff.snapshot();
        set_owner_unavailable(restore_error, true,
            handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined);
        invalidate_history("output-handoff-restore-failed");
        clear_frame_state();
        trace_skip("output-handoff-restore-failed");
        return true;
    }
    if (consumed_writer_terminal_sequence != 0) {
        TargetStateFactoryProbe::instance().consume_overlay_writer_witnesses_through(
            consumed_writer_terminal_sequence);
    }

    const auto handoff_before_service = m_output_handoff.snapshot();
    if (handoff_before_service.has_generation &&
        (control_generation != m_output_handoff_control_generation ||
            reset_generation != m_output_handoff_device_reset_generation)) {
        m_output_handoff.request_retirement(
            control_generation != m_output_handoff_control_generation
                ? "upscaling mode/quality generation changed"
                : "D3D12 device-reset generation changed");
    }

    RE4XeSSWorker::ControlRequest control_request{};
    control_request.active = sdk::GameIdentity::get().is_re4() && requested_mode != UpscalingMode::Off;
    control_request.mode_token = static_cast<int32_t>(requested_mode);
    control_request.quality = mode_to_quality_setting(requested_mode);
    control_request.control_generation = control_generation;
    control_request.device_reset_generation = reset_generation;
    control_request.caller_thread_id = callback_thread_id;
    control_request.external_fault = pending_worker_fault(control_generation, reset_generation);
    control_request.reframework_directory = reframework_module_directory();
    if (control_request.active) {
        (void)get_display_resolution(control_request.display);
    }
    if (g_framework != nullptr && g_framework->get_renderer_type() == REFramework::RendererType::D3D12) {
        const auto& hook = g_framework->get_d3d12_hook();
        if (hook != nullptr) {
            control_request.device = hook->get_device();
            control_request.queue = hook->get_command_queue();
        }
    }

    RE4XeSSWorker::ControlResult control_result{};
    const auto producer_before_service = get_producer_snapshot();
    const bool worker_already_idle_for_off =
        requested_mode == UpscalingMode::Off &&
        producer_before_service.mode == UpscalingMode::Off &&
        producer_before_service.control_generation == control_generation &&
        producer_before_service.device_reset_generation == reset_generation &&
        !producer_before_service.context_ready &&
        !producer_before_service.execution_ready &&
        !producer_before_service.draining &&
        !producer_before_service.faulted &&
        producer_before_service.bridge_idle &&
        !producer_before_service.bridge_quarantined;
    if (worker_already_idle_for_off) {
        control_result.status = RE4XeSSWorker::ServiceStatus::Waiting;
        control_result.control_generation = control_generation;
        control_result.device_reset_generation = reset_generation;
        auto& snapshot = control_result.snapshot;
        snapshot.mode_token = static_cast<int32_t>(UpscalingMode::Off);
        snapshot.control_generation = producer_before_service.control_generation;
        snapshot.device_reset_generation = producer_before_service.device_reset_generation;
        snapshot.bridge_idle = producer_before_service.bridge_idle;
        snapshot.bridge_quarantined = producer_before_service.bridge_quarantined;
        snapshot.bridge_device_removed = producer_before_service.bridge_device_removed;
        snapshot.device_identity = producer_before_service.device_identity;
        snapshot.queue_identity = producer_before_service.queue_identity;
        snapshot.failure_reason = producer_before_service.failure_reason;
    } else {
        control_result = m_worker.service_sync(control_request);
        publish_worker_snapshot(control_result.snapshot);
        if (control_result.status != RE4XeSSWorker::ServiceStatus::DispatchFailed &&
            control_result.status != RE4XeSSWorker::ServiceStatus::Stale &&
            control_result.control_generation == control_generation &&
            control_result.device_reset_generation == reset_generation) {
            acknowledge_worker_fault(control_generation, reset_generation);
        }
    }

    const auto control_still_current =
        control_result.control_generation == control_generation &&
        control_result.device_reset_generation == reset_generation &&
        m_control_generation.load(std::memory_order_acquire) == control_generation &&
        m_device_reset_generation.load(std::memory_order_acquire) == reset_generation &&
        m_requested_mode.load(std::memory_order_acquire) == requested_mode &&
        m_pre_overlay_overlap_epoch.load(std::memory_order_acquire) == overlap_epoch;
    const auto writer_status = m_output_handoff.poll_retirement(
        control_result.snapshot.bridge_idle,
        control_result.snapshot.bridge_device_removed);
    (void)writer_status;

    if (!control_still_current) {
        if (m_output_handoff.snapshot().has_generation) {
            m_output_handoff.request_retirement("pre-Overlay control result became stale");
            m_output_handoff.poll_retirement(
                control_result.snapshot.bridge_idle,
                control_result.snapshot.bridge_device_removed);
        }
        invalidate_history("pre-overlay-control-stale");
        clear_frame_state();
        trace_skip("pre-overlay-control-stale");
        return true;
    }

    if (control_result.status == RE4XeSSWorker::ServiceStatus::DispatchFailed ||
        control_result.status == RE4XeSSWorker::ServiceStatus::Faulted) {
        const auto reason = !control_result.snapshot.failure_reason.empty()
            ? control_result.snapshot.failure_reason
            : "The RE4XeSS worker could not service the current control request";
        if (m_output_handoff.snapshot().has_generation) {
            m_output_handoff.request_retirement("RE4XeSS worker control dispatch/fault");
            m_output_handoff.poll_retirement(
                control_result.snapshot.bridge_idle,
                control_result.snapshot.bridge_device_removed);
        }
        set_owner_unavailable(reason, control_result.snapshot.draining, true);
        invalidate_history("worker-control-fault");
        clear_frame_state();
        trace_skip("worker-control-fault");
        return true;
    }

    const bool worker_ready = control_result.status == RE4XeSSWorker::ServiceStatus::Ready &&
        control_result.ready_for_frame &&
        control_result.snapshot.context_ready &&
        !control_result.snapshot.draining &&
        !control_result.snapshot.faulted;
    const bool worker_configuration_matches = !control_result.ready_for_frame ||
        (control_result.snapshot.mode_token == control_request.mode_token &&
            control_request.quality.has_value() &&
            control_result.snapshot.quality == *control_request.quality &&
            control_result.snapshot.display.x == control_request.display.x &&
            control_result.snapshot.display.y == control_request.display.y &&
            control_result.snapshot.device_identity == reinterpret_cast<uintptr_t>(control_request.device.Get()) &&
            control_result.snapshot.queue_identity == reinterpret_cast<uintptr_t>(control_request.queue.Get()));
    if (worker_ready && !worker_configuration_matches) {
        const std::string reason{ "RE4XeSS worker reported readiness for a different mode/device/queue/display configuration" };
        mark_execution_fault(reason);
        set_owner_unavailable(reason, false, true);
        invalidate_history("worker-configuration-mismatch");
        clear_frame_state();
        trace_skip("worker-configuration-mismatch");
        return true;
    }
    if (!worker_ready) {
        if (m_output_handoff.snapshot().has_generation) {
            m_output_handoff.request_retirement("XeSS worker configuration is not ready for this frame");
            m_output_handoff.poll_retirement(
                control_result.snapshot.bridge_idle,
                control_result.snapshot.bridge_device_removed);
        }
        clear_frame_state();
        trace_skip("worker-not-ready");
        return true;
    }

    if (!is_temporal_active()) {
        m_output_handoff.request_retirement("temporal upscaling mode is inactive or stale");
        m_output_handoff.poll_retirement(
            control_result.snapshot.bridge_idle,
            control_result.snapshot.bridge_device_removed);
        clear_frame_state();
        trace_skip("temporal-mode-inactive");
        return true;
    }
    load_window = begin_load_state_trace();
    finish_load_state_trace(load_window);
    const bool load_admitted = RE4XeSSLoadEligibility::allows_temporal_rendering(load_window.state);
    trace_load_state_admission(3, !load_window.state.observation_valid
            ? "observation-unavailable"
            : load_admitted ? "eligible" : "native-load-window",
        load_window);
    if (!load_window.state.observation_valid) {
        invalidate_history("load-state-observation-unavailable");
        clear_frame_state();
        trace_skip("load-state-observation-unavailable");
        return true;
    }
    if (!load_admitted) {
        invalidate_history("load-history-invalid");
        clear_frame_state();
        trace_skip("load-history-invalid");
        return true;
    }
    if (layer == nullptr) {
        invalidate_history("pre-overlay-layer-unavailable");
        clear_frame_state();
        trace_skip("pre-overlay-layer-unavailable");
        return true;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame || !m_cached_scene || !m_cached_scene_frame ||
        *m_cached_scene_frame != static_cast<uint64_t>(*frame)) {
        invalidate_history("pre-overlay-primary-scene-frame-missing");
        clear_frame_state();
        trace_skip("pre-overlay-primary-scene-frame-missing");
        return true;
    }

    if (!m_camera_metadata_valid || !m_camera_frame || *m_camera_frame != *m_cached_scene_frame) {
        invalidate_history("pre-overlay-temporal-gate-invalid");
        clear_frame_state();
        trace_skip("pre-overlay-temporal-gate-invalid");
        return true;
    }

    auto* color_state = layer->get_main_target_state().get();
    auto* color = color_state != nullptr ? color_state->get_native_resource_d3d12() : nullptr;
    auto* scene = m_cached_scene;
    auto* depth = scene->get_depth_stencil_d3d12();
    auto* velocity = scene->get_motion_vectors_d3d12();
    if (color == nullptr || depth == nullptr || velocity == nullptr ||
        color != scene->get_post_main_target_d3d12() || color != scene->get_hdr_target_d3d12()) {
        invalidate_history("temporal-resource-identity-invariant-failed");
        clear_frame_state();
        trace_skip("temporal-resource-identity-invariant-failed");
        return true;
    }

    const auto render_width = m_input_resolution.optimal.x;
    const auto render_height = m_input_resolution.optimal.y;
    if (!is_valid_texture_extent(color, render_width, render_height) ||
        !is_valid_texture_extent(depth, render_width, render_height) ||
        !is_valid_texture_extent(velocity, render_width, render_height)) {
        invalidate_history("temporal-resource-extent-invariant-failed");
        clear_frame_state();
        trace_skip("temporal-resource-extent-invariant-failed");
        return true;
    }
    if (color->GetDesc().Format != DXGI_FORMAT_R11G11B10_FLOAT) {
        const std::string reason{ "The current RE4 Color resource is not R11G11B10_FLOAT; PR3 output contract cannot be met" };
        mark_execution_fault(reason);
        set_owner_unavailable(reason, false, true);
        invalidate_history("unsupported-color-format");
        clear_frame_state();
        trace_skip("unsupported-color-format");
        return true;
    }

    const auto color_identity = reinterpret_cast<uintptr_t>(color);
    const auto depth_identity = reinterpret_cast<uintptr_t>(depth);
    const auto velocity_identity = reinterpret_cast<uintptr_t>(velocity);
    const bool resource_identity_changed = m_resource_identity_valid &&
        (color_identity != m_last_color_identity ||
            depth_identity != m_last_depth_identity ||
            velocity_identity != m_last_velocity_identity);
    if (resource_identity_changed) {
        invalidate_history("temporal-resource-identity-change");
    }

    RE4XeSSFrame packet{};
    packet.color = color;
    packet.depth = depth;
    packet.velocity = velocity;
    packet.render_width = render_width;
    packet.render_height = render_height;
    packet.display_width = m_display_resolution.x;
    packet.display_height = m_display_resolution.y;
    packet.jitter_x_pixels = m_cached_jitter_x;
    packet.jitter_y_pixels = m_cached_jitter_y;
    packet.motion_scale_x = static_cast<float>(render_width) / 2.0f;
    packet.motion_scale_y = -static_cast<float>(render_height) / 2.0f;
    packet.near_plane = m_camera_near;
    packet.far_plane = m_camera_far;
    packet.vertical_fov = m_cached_vertical_fov;
    packet.reset_history = m_first_valid_frame_reset_pending || m_history_invalid;
    packet.frame_id = *m_cached_scene_frame;
    packet.lifetime_trace_id = trace_id;

    if (packet.reset_history && debug_log) {
        spdlog::info("[RE4XeSS][Frame] first valid resetHistory packet: frame={} input={}x{} display={}x{}",
            static_cast<unsigned long long>(packet.frame_id),
            packet.render_width,
            packet.render_height,
            packet.display_width,
            packet.display_height);
    }

    const RE4XeSSD3D12::Signature bridge_signature{
        packet.render_width,
        packet.render_height,
        packet.display_width,
        packet.display_height,
        color->GetDesc().Format,
    };
    RE4XeSSD3D12::OutputBinding output{};
    std::string handoff_error;
    const auto handoff_prepare_result = m_output_handoff.prepare(
        layer,
        control_request.device.Get(),
        control_request.queue.Get(),
        color,
        render_width,
        render_height,
        packet.display_width,
        packet.display_height,
        control_generation,
        control_result.snapshot.bridge_idle,
        control_result.snapshot.bridge_device_removed,
        output,
        handoff_error);
    if (handoff_prepare_result != RE4XeSSOutputHandoff::PrepareResult::Ready) {
        if (handoff_prepare_result == RE4XeSSOutputHandoff::PrepareResult::WaitingForPostPresentMarker) {
            invalidate_history("output-handoff-marker-pending");
            clear_frame_state();
            trace_skip("output-handoff-marker-pending");
            if (lifetime_trace.claim_anomaly_window(
                    RE4XeSSLifetimeTrace::AnomalyWindow::FirstMarkerPending)) {
                dump_re4_xess_lifetime_trace(
                    "first-output-handoff-marker-pending",
                    32,
                    RE4XeSSLifetimeTrace::DumpWindow::Anomaly);
            }
            return true;
        }

        const auto handoff_state = m_output_handoff.snapshot();
        const bool retirement_pending =
            handoff_state.retirement == RE4XeSSOutputHandoff::RetirementStatus::WriterPending ||
            handoff_state.retirement == RE4XeSSOutputHandoff::RetirementStatus::MissingMarker ||
            handoff_state.retirement == RE4XeSSOutputHandoff::RetirementStatus::Draining ||
            handoff_state.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined;
        if (retirement_pending) {
            set_owner_unavailable(handoff_error, true,
                handoff_state.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined);
        } else {
            mark_execution_fault(handoff_error);
            set_owner_unavailable(handoff_error, false, true);
        }
        invalidate_history("output-handoff-unavailable");
        clear_frame_state();
        trace_skip(handoff_error.empty() ? "output-handoff-unavailable" : std::string_view{ handoff_error });
        if (handoff_state.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined &&
            lifetime_trace.claim_anomaly_window(
                RE4XeSSLifetimeTrace::AnomalyWindow::WriterOrFenceFailure)) {
            dump_re4_xess_lifetime_trace(
                "output-handoff-quarantined",
                32,
                RE4XeSSLifetimeTrace::DumpWindow::Anomaly);
        }
        return true;
    }
    m_output_handoff_control_generation = control_generation;
    m_output_handoff_device_reset_generation = reset_generation;

    RE4XeSSWorker::SubmitRequest submit_request{};
    submit_request.frame = packet;
    submit_request.color_pin = color;
    submit_request.depth_pin = depth;
    submit_request.velocity_pin = velocity;
    submit_request.output = output;
    submit_request.output_pin = output.resource;
    submit_request.bridge_signature = bridge_signature;
    submit_request.mode_token = static_cast<int32_t>(requested_mode);
    submit_request.control_generation = control_generation;
    submit_request.device_reset_generation = reset_generation;
    submit_request.caller_thread_id = callback_thread_id;

    const auto submit_result = m_worker.submit_sync(std::move(submit_request));
    publish_worker_snapshot(submit_result.snapshot);
    const auto handoff_after_submit = m_output_handoff.snapshot();
    RE4XeSSLifetimeTrace::Event submit_event{};
    submit_event.kind = RE4XeSSLifetimeTrace::Kind::Submit;
    submit_event.trace_id = trace_id;
    submit_event.frame_id = packet.frame_id;
    submit_event.frame_valid = true;
    submit_event.callback_ordinal = callback_ordinal;
    submit_event.submit_ordinal = submit_result.submit_ordinal;
    submit_event.present_ordinal = present_is_current() ? present_diagnostics.ordinal : 0;
    submit_event.output_generation = handoff_after_submit.output_generation;
    submit_event.control_generation = submit_result.control_generation;
    submit_event.device_reset_generation = submit_result.device_reset_generation;
    submit_event.bridge_slot = submit_result.bridge_slot;
    submit_event.writer_fence_value = submit_result.writer_fence_value;
    submit_event.output_resource = reinterpret_cast<uintptr_t>(output.resource);
    submit_event.target_state = handoff_after_submit.target_state;
    submit_event.overlay = reinterpret_cast<uintptr_t>(layer);
    submit_event.swapchain = present_diagnostics.swapchain;
    submit_event.device = reinterpret_cast<uintptr_t>(control_request.device.Get());
    submit_event.queue = reinterpret_cast<uintptr_t>(control_request.queue.Get());
    submit_event.command_queue_type = control_request.queue != nullptr
        ? static_cast<int32_t>(control_request.queue->GetDesc().Type) : -1;
    submit_event.command_queue_type_valid = control_request.queue != nullptr;
    submit_event.thread_id = callback_thread_id;
    submit_event.bridge_slot = submit_result.bridge_slot;
    submit_event.result = static_cast<int32_t>(submit_result.status);
    submit_event.frame_valid = true;
    submit_event.present_valid = present_is_current();
    submit_event.reset_history = packet.reset_history;
    submit_event.api_succeeded = submit_result.execute_api_succeeded;
    submit_event.queue_submitted = submit_result.queue_submitted;
    submit_event.writer_signal_succeeded = submit_result.writer_signal_succeeded;
    submit_event.successful_submission = submit_result.status == RE4XeSSWorker::SubmitResult::Status::Submitted &&
        submit_result.execute_api_succeeded && submit_result.queue_submitted && submit_result.writer_signal_succeeded;
    const char* submit_reason = "dispatch-failed";
    switch (submit_result.status) {
    case RE4XeSSWorker::SubmitResult::Status::Submitted: submit_reason = "submitted"; break;
    case RE4XeSSWorker::SubmitResult::Status::Busy: submit_reason = "busy"; break;
    case RE4XeSSWorker::SubmitResult::Status::Faulted: submit_reason = "faulted"; break;
    case RE4XeSSWorker::SubmitResult::Status::NotReady: submit_reason = "not-ready"; break;
    case RE4XeSSWorker::SubmitResult::Status::Stale: submit_reason = "stale"; break;
    case RE4XeSSWorker::SubmitResult::Status::DispatchFailed: submit_reason = "dispatch-failed"; break;
    }
    RE4XeSSLifetimeTrace::set_reason(submit_event, submit_reason);
    lifetime_trace.record(submit_event);
    if (submit_event.successful_submission &&
        lifetime_trace.claim_first_execute_success_window()) {
        dump_re4_xess_lifetime_trace(
            "first-public-xess-execute-success",
            32,
            RE4XeSSLifetimeTrace::DumpWindow::ActiveMilestone);
    }
    if (submit_event.successful_submission) {
        const auto submitted = m_lifetime_submit_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (submitted % 1024 == 0) {
            dump_re4_xess_lifetime_trace(
                "submit-checkpoint",
                32,
                RE4XeSSLifetimeTrace::DumpWindow::Periodic);
        }
    }
    if (submit_result.status == RE4XeSSWorker::SubmitResult::Status::Submitted) {
        m_output_handoff.note_submission_succeeded();

        const auto submit_still_current =
            submit_result.control_generation == control_generation &&
            submit_result.device_reset_generation == reset_generation &&
            m_control_generation.load(std::memory_order_acquire) == control_generation &&
            m_device_reset_generation.load(std::memory_order_acquire) == reset_generation &&
            m_requested_mode.load(std::memory_order_acquire) == requested_mode &&
            m_pre_overlay_overlap_epoch.load(std::memory_order_acquire) == overlap_epoch;
        if (!submit_still_current) {
            m_output_handoff.request_retirement("submitted XeSS output belongs to a stale control/device/overlap generation");
            m_output_handoff.poll_retirement(
                submit_result.bridge_idle,
                submit_result.bridge_device_removed);
            invalidate_history("xess-submit-stale-after-submit");
            clear_frame_state();
            trace_skip("xess-submit-stale-after-submit");
            return true;
        }

        std::string install_error;
        const RE4XeSSOutputHandoff::ObservationContext install_observation{
            static_cast<int32_t>(requested_mode),
            control_generation,
            reset_generation,
            packet.frame_id,
            callback_thread_id,
            true,
            trace_id,
            callback_ordinal,
            present_is_current() ? present_diagnostics.ordinal : 0,
            submit_result.submit_ordinal,
            submit_result.writer_fence_value,
            present_diagnostics.swapchain,
            reinterpret_cast<uintptr_t>(control_request.device.Get()),
            reinterpret_cast<uintptr_t>(control_request.queue.Get()),
            submit_result.bridge_slot,
            present_is_current(),
            control_request.queue != nullptr
                ? static_cast<int32_t>(control_request.queue->GetDesc().Type) : -1,
            control_request.queue != nullptr,
        };
        if (!m_output_handoff.install(layer, install_observation, install_error)) {
            m_output_handoff.request_retirement("XeSS output was submitted but Overlay installation failed");
            set_owner_unavailable(install_error, true, true);
            invalidate_history("output-handoff-install-failed");
            clear_frame_state();
            trace_skip("output-handoff-install-failed");
            return true;
        }

        if (trace_enabled) {
            const auto installed = m_output_handoff.snapshot();
            const auto context_target = snapshot_render_context_target(render_context);
            const auto& overlay_main = layer->get_main_target_state();
            const auto overlay_main_state = reinterpret_cast<uintptr_t>(overlay_main.get());
            const auto overlay_main_resource = overlay_main != nullptr
                ? reinterpret_cast<uintptr_t>(overlay_main->get_native_resource_d3d12())
                : 0;
            RE4XeSSLifetimeTrace::Event context_event{};
            context_event.kind = RE4XeSSLifetimeTrace::Kind::OverlayRenderContext;
            context_event.render_context_stage = RE4XeSSLifetimeTrace::RenderContextStage::BeforeOriginalOverlayDraw;
            context_event.trace_id = trace_id;
            context_event.install_id = installed.install_id;
            context_event.frame_id = packet.frame_id;
            context_event.frame_valid = true;
            context_event.callback_ordinal = callback_ordinal;
            context_event.present_ordinal = present_is_current() ? present_diagnostics.ordinal : 0;
            context_event.output_generation = installed.output_generation;
            context_event.control_generation = control_generation;
            context_event.device_reset_generation = reset_generation;
            context_event.output_resource = installed.output_resource;
            context_event.target_state = installed.target_state;
            context_event.render_context = context_target.context;
            context_event.render_context_target_state = context_target.target_state;
            context_event.render_context_target_resource = context_target.target_resource;
            context_event.render_context_sample_valid = context_target.valid;
            context_event.render_context_target_state_matches_output =
                context_target.target_state != 0 && context_target.target_state == installed.target_state;
            context_event.render_context_target_resource_matches_output =
                context_target.target_resource != 0 && context_target.target_resource == installed.output_resource;
            context_event.overlay_main_target_state = overlay_main_state;
            context_event.overlay_main_target_resource = overlay_main_resource;
            context_event.render_context_target_state_matches_overlay_main =
                context_target.target_state != 0 && context_target.target_state == overlay_main_state;
            context_event.render_context_target_resource_matches_overlay_main =
                context_target.target_resource != 0 && context_target.target_resource == overlay_main_resource;
            context_event.overlay = reinterpret_cast<uintptr_t>(layer);
            context_event.swapchain = present_diagnostics.swapchain;
            context_event.device = reinterpret_cast<uintptr_t>(control_request.device.Get());
            context_event.queue = reinterpret_cast<uintptr_t>(control_request.queue.Get());
            context_event.command_queue_type = control_request.queue != nullptr
                ? static_cast<int32_t>(control_request.queue->GetDesc().Type) : -1;
            context_event.command_queue_type_valid = control_request.queue != nullptr;
            context_event.thread_id = callback_thread_id;
            context_event.present_entry_thread_id = present_diagnostics.entry_thread_id;
            context_event.present_return_thread_id = present_diagnostics.return_thread_id;
            context_event.present_source = static_cast<int32_t>(present_diagnostics.source);
            context_event.present1 = present_diagnostics.present1;
            context_event.present_returned = present_diagnostics.returned;
            context_event.present_callbacks_suppressed = present_diagnostics.render_callbacks_suppressed;
            context_event.original_present_skipped = present_diagnostics.original_call_skipped;
            context_event.present_valid = present_is_current();
            RE4XeSSLifetimeTrace::set_reason(context_event,
                !context_target.valid ? "pre-original-render-context-null" :
                context_target.target_state == 0 ? "pre-original-context-target-null" :
                context_target.target_resource == 0 ? "pre-original-context-native-resource-null" :
                context_event.render_context_target_resource_matches_output
                    ? "before-original-context-output-resource-match-not-submit-proof" :
                context_event.render_context_target_state_matches_output
                    ? "before-original-context-targetstate-match-not-submit-proof" :
                context_event.render_context_target_resource_matches_overlay_main
                    ? "pre-original-context-resource-matches-overlay-main-not-submit-proof" :
                context_event.render_context_target_state_matches_overlay_main
                    ? "pre-original-context-targetstate-matches-overlay-main-not-submit-proof" :
                "pre-original-overlay-draw-context-snapshot");
            lifetime_trace.record(context_event);
        }

        if (lifetime_trace.claim_first_output_install_window()) {
            dump_re4_xess_lifetime_trace(
                "first-successful-output-install",
                32,
                RE4XeSSLifetimeTrace::DumpWindow::ActiveMilestone);
        }
        if (lifetime_trace.claim_first_execute_success_window()) {
            dump_re4_xess_lifetime_trace(
                "first-public-xess-execute-success",
                32,
                RE4XeSSLifetimeTrace::DumpWindow::ActiveMilestone);
        }

        m_last_color_identity = color_identity;
        m_last_depth_identity = depth_identity;
        m_last_velocity_identity = velocity_identity;
        m_resource_identity_valid = true;
        m_latest_frame_snapshot = FrameSnapshot{
            packet.frame_id,
            color_identity,
            depth_identity,
            velocity_identity,
            packet.render_width,
            packet.render_height,
            packet.display_width,
            packet.display_height,
            packet.jitter_x_pixels,
            packet.jitter_y_pixels,
            packet.motion_scale_x,
            packet.motion_scale_y,
            packet.near_plane,
            packet.far_plane,
            packet.vertical_fov,
            packet.reset_history,
        };
        m_first_valid_frame_reset_pending = false;
        m_history_invalid = false;
        m_last_reset_reason.clear();
        (void)record_lifetime_event(RE4XeSSLifetimeTrace::Kind::PreOverlay, "submit-and-install-complete");
    } else if (submit_result.status == RE4XeSSWorker::SubmitResult::Status::Busy ||
        submit_result.status == RE4XeSSWorker::SubmitResult::Status::Stale ||
        submit_result.status == RE4XeSSWorker::SubmitResult::Status::NotReady) {
        invalidate_history(submit_result.status == RE4XeSSWorker::SubmitResult::Status::Busy
                ? "xess-command-ring-busy"
            : "xess-submit-stale");
        trace_skip(submit_result.status == RE4XeSSWorker::SubmitResult::Status::Busy
            ? "xess-command-ring-busy"
            : "xess-submit-stale-or-not-ready");
        if (submit_result.status == RE4XeSSWorker::SubmitResult::Status::Busy && debug_log) {
            static uint32_t busy_warning_count{};
            if (busy_warning_count < 8) {
                ++busy_warning_count;
                spdlog::warn("[RE4XeSS][Failure] all bridge slots are busy; skipping frame and resetting XeSS history");
            }
        }
        if (submit_result.status == RE4XeSSWorker::SubmitResult::Status::Stale) {
            m_output_handoff.request_retirement("worker rejected a stale submit generation");
            m_output_handoff.poll_retirement(
                submit_result.bridge_idle,
                submit_result.bridge_device_removed);
        }
    } else {
        const auto reason = !submit_result.failure_reason.empty()
            ? submit_result.failure_reason
            : "The RE4XeSS worker failed to submit the current frame";
        if (submit_result.status == RE4XeSSWorker::SubmitResult::Status::DispatchFailed ||
            submit_result.bridge_quarantined ||
            !submit_result.bridge_idle) {
            m_output_handoff.quarantine(reason, true);
        } else {
            m_output_handoff.request_retirement("XeSS submit failed before GPU work became active");
            m_output_handoff.poll_retirement(
                submit_result.bridge_idle,
                submit_result.bridge_device_removed);
        }
        mark_execution_fault(reason);
        set_owner_unavailable(reason, submit_result.snapshot.draining, true);
        invalidate_history("xess-execute-fault");
        trace_skip("xess-execute-fault");
    }

    clear_frame_state();
    return true;
}



void RE4XeSS::on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    if (m_output_handoff.identity_mismatch_latched()) {
        return;
    }
    const auto callback_thread_id = GetCurrentThreadId();
    const auto trace_enabled = RE4XeSSLifetimeTrace::instance().enabled();
    D3D12Hook::PresentDiagnosticsSnapshot present{};
    if (trace_enabled && g_framework != nullptr &&
        g_framework->get_renderer_type() == REFramework::RendererType::D3D12) {
        const auto& hook = g_framework->get_d3d12_hook();
        if (hook != nullptr) {
            present = hook->get_present_diagnostics_snapshot();
        }
    }
    const auto present_valid = present.active && present.ordinal != 0 &&
        present.entry_thread_id == callback_thread_id;
    const auto callback_ordinal = trace_enabled
        ? m_post_overlay_lifetime_ordinal.fetch_add(1, std::memory_order_relaxed) + 1
        : 0;
    const RE4XeSSOutputHandoff::ObservationContext observation{
        static_cast<int32_t>(m_requested_mode.load(std::memory_order_acquire)),
        m_control_generation.load(std::memory_order_acquire),
        m_device_reset_generation.load(std::memory_order_acquire),
        0,
        callback_thread_id,
        false,
        0,
        callback_ordinal,
        present_valid ? present.ordinal : 0,
        0,
        0,
        present.swapchain,
        present.device,
        present.command_queue,
        0,
        present_valid,
    };
    const auto before_observation = trace_enabled ? m_output_handoff.snapshot()
        : RE4XeSSOutputHandoff::Snapshot{};
    uintptr_t overlay_main_state_before_observation{};
    uintptr_t overlay_main_resource_before_observation{};
    if (trace_enabled && before_observation.installed) {
        const auto& overlay_main_before_observation = layer->get_main_target_state();
        overlay_main_state_before_observation = reinterpret_cast<uintptr_t>(overlay_main_before_observation.get());
        if (overlay_main_before_observation != nullptr) {
            overlay_main_resource_before_observation = reinterpret_cast<uintptr_t>(
                overlay_main_before_observation->get_native_resource_d3d12());
        }
    }
    const auto context_target = trace_enabled && before_observation.installed
        ? snapshot_render_context_target(render_context)
        : RenderContextTargetSnapshot{};
    const auto writer_witness_chain =
        TargetStateFactoryProbe::instance().confirmed_overlay_writer_witness_chain();
    m_output_handoff.observe_overlay(
        layer,
        observation,
        writer_witness_chain);
    if (trace_enabled) {
        const auto after_observation = m_output_handoff.snapshot();
        RE4XeSSLifetimeTrace::Event event{};
        event.kind = RE4XeSSLifetimeTrace::Kind::PostOverlayObservation;
        event.trace_id = before_observation.installed_trace_id;
        event.install_id = before_observation.install_id;
        event.frame_id = before_observation.installed_frame;
        event.frame_valid = before_observation.installed_trace_id != 0;
        event.callback_ordinal = callback_ordinal;
        event.present_ordinal = present_valid ? present.ordinal : 0;
        event.related_present_ordinal = before_observation.installed_present_ordinal;
        event.output_generation = before_observation.output_generation;
        event.control_generation = before_observation.installed_control_generation;
        event.device_reset_generation = before_observation.installed_device_reset_generation;
        if (writer_witness_chain.count > 0) {
            const auto& writer_witness = writer_witness_chain.entries[writer_witness_chain.count - 1];
            event.writer_sequence = writer_witness.sequence;
            event.writer_invocation_id = writer_witness.invocation_id;
            event.writer_clear_pre_sequence = writer_witness.clear_pre_sequence;
            event.writer_clear_post_sequence = writer_witness.clear_post_sequence;
            event.writer_replacement_pre_sequence = writer_witness.replacement_pre_sequence;
            event.writer_replacement_post_sequence = writer_witness.replacement_post_sequence;
            event.writer_method_rva = writer_witness.method_rva;
            event.writer_transaction_confirmed = writer_witness.same_invocation &&
                writer_witness.clear_write_confirmed &&
                writer_witness.replacement_write_confirmed &&
                writer_witness.re4_image_identity_verified;
        }
        event.downstream_fence_value = before_observation.last_signaled_fence_value;
        event.output_resource = before_observation.output_resource;
        event.target_state = reinterpret_cast<uintptr_t>(layer->get_main_target_state().get());
        event.render_context_stage = RE4XeSSLifetimeTrace::RenderContextStage::PostOverlayCallbackOriginalStatusUnknown;
        event.render_context = context_target.context;
        event.render_context_target_state = context_target.target_state;
        event.render_context_target_resource = context_target.target_resource;
        event.render_context_sample_valid = context_target.valid;
        event.render_context_target_state_matches_output = context_target.target_state != 0 &&
            context_target.target_state == before_observation.target_state;
        event.render_context_target_resource_matches_output = context_target.target_resource != 0 &&
            context_target.target_resource == before_observation.output_resource;
        event.overlay_main_target_state = overlay_main_state_before_observation;
        event.overlay_main_target_resource = overlay_main_resource_before_observation;
        event.render_context_target_state_matches_overlay_main = context_target.target_state != 0 &&
            context_target.target_state == overlay_main_state_before_observation;
        event.render_context_target_resource_matches_overlay_main = context_target.target_resource != 0 &&
            context_target.target_resource == overlay_main_resource_before_observation;
        event.overlay = reinterpret_cast<uintptr_t>(layer);
        event.swapchain = present.swapchain;
        event.device = present.device;
        event.queue = present.command_queue;
        event.command_queue_type = present.command_queue_type;
        event.command_queue_type_valid = present.command_queue_type_valid;
        event.thread_id = callback_thread_id;
        event.present_entry_thread_id = present.entry_thread_id;
        event.present_return_thread_id = present.return_thread_id;
        event.present_source = static_cast<int32_t>(present.source);
        event.result = present.result_valid ? static_cast<int32_t>(present.result) : E_PENDING;
        event.present_valid = present_valid;
        event.present_returned = present.returned;
        event.present1 = present.present1;
        event.present_callbacks_suppressed = present.render_callbacks_suppressed;
        event.original_present_skipped = present.original_call_skipped;
        event.mapping_ambiguous = before_observation.installed;
        const bool state_matches_handoff = before_observation.installed &&
            event.target_state == before_observation.target_state;
        RE4XeSSLifetimeTrace::set_reason(event,
            !before_observation.installed ? "overlay-observed-no-installed-output" :
            !context_target.valid ? "post-overlay-callback-render-context-null-original-status-unknown" :
            context_target.target_state == 0 ? "post-overlay-callback-context-target-null-original-status-unknown" :
            context_target.target_resource == 0 ? "post-overlay-callback-native-resource-null-original-status-unknown" :
            event.render_context_target_resource_matches_output
                ? (state_matches_handoff
                    ? "post-overlay-callback-context-output-match-not-submit-proof"
                    : "post-overlay-callback-context-output-match-slot-diverged") :
            event.render_context_target_state_matches_output
                ? "post-overlay-callback-context-targetstate-match-not-submit-proof" :
            event.render_context_target_resource_matches_overlay_main
                ? "post-overlay-callback-context-resource-matches-overlay-main-not-submit-proof" :
            event.render_context_target_state_matches_overlay_main
                ? "post-overlay-callback-context-targetstate-matches-overlay-main-not-submit-proof" :
            !state_matches_handoff ? "overlay-target-differs-from-handoff-not-reader-proof" :
            !present_valid ? "handoff-target-observed-present-context-unknown-not-reader-proof" :
            "handoff-target-observed-not-gpu-reader-proof");
        RE4XeSSLifetimeTrace::instance().record(event);
        if (before_observation.installed != after_observation.installed ||
            before_observation.target_state != after_observation.target_state ||
            before_observation.marker_pending != after_observation.marker_pending) {
            RE4XeSSLifetimeTrace::Event changed = event;
            changed.output_generation = after_observation.output_generation;
            changed.target_state = after_observation.target_state;
            changed.downstream_fence_value = after_observation.last_signaled_fence_value;
            RE4XeSSLifetimeTrace::set_reason(changed, "overlay-observation-state-changed");
            RE4XeSSLifetimeTrace::instance().record(changed);
        }
    }
}

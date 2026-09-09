#pragma once

#include <mutex>
#include <optional>

#include "XeFGDiscovery.hpp"

class D3D12Hook;

class XeFGCandidateHandoff {
public:
    static void publish(XeFGBindingCandidate candidate);
    static bool consume_pending(D3D12Hook& hook);
    static bool discard_pending_for_runtime_transition(
        size_t runtime_slot,
        void* context,
        HWND hwnd,
        bool allow_same_hwnd_match,
        const char* reason);

private:
    static void store_pending(XeFGBindingCandidate candidate);
    static void apply_to_live_hook(D3D12Hook& hook, const XeFGBindingCandidate& candidate);

    static std::mutex s_pending_mutex;
    static std::optional<XeFGBindingCandidate> s_pending_candidate;
};

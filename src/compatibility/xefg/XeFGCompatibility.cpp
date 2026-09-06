#include "XeFGCompatibility.hpp"

#include <chrono>
#include <tlhelp32.h>

#include <spdlog/spdlog.h>

#include "XeFGRuntimeRegistry.hpp"
#include "utility/String.hpp"

namespace {
const auto g_diagnostic_start_time = std::chrono::steady_clock::now();
}

std::atomic<bool> XeFGCompatibility::s_module_loaded{false};
std::atomic<bool> XeFGCompatibility::s_probe_pending{false};
std::atomic<int64_t> XeFGCompatibility::s_first_seen_ms{-1};

void XeFGCompatibility::mark_probe_pending() noexcept {
    s_module_loaded.store(true, std::memory_order_release);
    const auto observed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_diagnostic_start_time).count();
    int64_t expected = -1;
    s_first_seen_ms.compare_exchange_strong(expected, observed_ms, std::memory_order_relaxed);
    s_probe_pending.store(true, std::memory_order_release);
}

void XeFGCompatibility::on_module_loaded(HMODULE module, std::wstring_view base_name, std::wstring_view full_path) {
    if (module == nullptr) {
        return;
    }

    s_module_loaded.store(true, std::memory_order_relaxed);
    const auto observed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_diagnostic_start_time).count();
    int64_t expected = -1;
    s_first_seen_ms.compare_exchange_strong(expected, observed_ms, std::memory_order_relaxed);

    spdlog::info("[XeFG][Module] name = {}, base = 0x{:x}, full_path = {}, first_seen_ms = {}",
        utility::narrow(std::wstring{base_name}), reinterpret_cast<uintptr_t>(module),
        utility::narrow(std::wstring{full_path}), s_first_seen_ms.load(std::memory_order_relaxed));

    const auto init_from_swap_chain = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChain");
    const auto init_from_swap_chain_desc = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChainDesc");
    const auto get_swap_chain_ptr = GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr");
    spdlog::info("[XeFG][Exports] InitFromSwapChain = 0x{:x} / {}, InitFromSwapChainDesc = 0x{:x} / {}, GetSwapChainPtr = 0x{:x} / {}",
        reinterpret_cast<uintptr_t>(init_from_swap_chain), init_from_swap_chain != nullptr ? "present" : "missing",
        reinterpret_cast<uintptr_t>(init_from_swap_chain_desc), init_from_swap_chain_desc != nullptr ? "present" : "missing",
        reinterpret_cast<uintptr_t>(get_swap_chain_ptr), get_swap_chain_ptr != nullptr ? "present" : "missing");

    XeFGRuntimeRegistry::instance().install_for_module(module, full_path);
}

void XeFGCompatibility::install_already_loaded_runtimes() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        spdlog::warn("[XeFG][RuntimeRegistry] module enumeration failed, error = {}", GetLastError());
        return;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"libxess_fg.dll") == 0) {
                on_module_loaded(entry.hModule, std::wstring_view{entry.szModule}, std::wstring_view{entry.szExePath});
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void XeFGCompatibility::process_pending_work() {
    if (!s_probe_pending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    install_already_loaded_runtimes();
}

bool XeFGCompatibility::is_module_loaded() noexcept {
    return s_module_loaded.load(std::memory_order_relaxed);
}

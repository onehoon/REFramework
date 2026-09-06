#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include <windows.h>

class XeFGCompatibility {
public:
    static void mark_probe_pending() noexcept;
    static void on_module_loaded(HMODULE module, std::wstring_view base_name, std::wstring_view full_path);
    static void process_pending_work();
    static void install_already_loaded_runtimes();
    static bool is_module_loaded() noexcept;

private:
    static std::atomic<bool> s_module_loaded;
    static std::atomic<bool> s_probe_pending;
    static std::atomic<int64_t> s_first_seen_ms;
};

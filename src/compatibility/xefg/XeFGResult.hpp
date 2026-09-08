#pragma once

#include <cstdint>

namespace xefg_result {

constexpr bool succeeded(int32_t result) noexcept {
    return result >= 0;
}

constexpr bool failed(int32_t result) noexcept {
    return result < 0;
}

} // namespace xefg_result

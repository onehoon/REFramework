#pragma once

#include <cstdint>

#include <d3d12.h>

// Minimal public XeSS ABI declarations used by the RE4 producer. Keep these
// values and signatures aligned with Intel's public xess.h/xess_d3d12.h API.
extern "C" {

typedef struct _xess_context_handle_t* xess_context_handle_t;

#pragma pack(push, 8)
typedef struct _xess_version_t {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t reserved;
} xess_version_t;

typedef struct _xess_2d_t {
    uint32_t x;
    uint32_t y;
} xess_2d_t;
#pragma pack(pop)

typedef enum _xess_quality_settings_t : int32_t {
    XESS_QUALITY_SETTING_ULTRA_PERFORMANCE = 100,
    XESS_QUALITY_SETTING_PERFORMANCE = 101,
    XESS_QUALITY_SETTING_BALANCED = 102,
    XESS_QUALITY_SETTING_QUALITY = 103,
    XESS_QUALITY_SETTING_ULTRA_QUALITY = 104,
    XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS = 105,
    XESS_QUALITY_SETTING_AA = 106,
} xess_quality_settings_t;

typedef enum _xess_result_t : int32_t {
    XESS_RESULT_WARNING_NONEXISTING_FOLDER = 1,
    XESS_RESULT_WARNING_OLD_DRIVER = 2,
    XESS_RESULT_SUCCESS = 0,
    XESS_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
    XESS_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
    XESS_RESULT_ERROR_UNINITIALIZED = -3,
    XESS_RESULT_ERROR_INVALID_ARGUMENT = -4,
    XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY = -5,
    XESS_RESULT_ERROR_DEVICE = -6,
    XESS_RESULT_ERROR_NOT_IMPLEMENTED = -7,
    XESS_RESULT_ERROR_INVALID_CONTEXT = -8,
    XESS_RESULT_ERROR_OPERATION_IN_PROGRESS = -9,
    XESS_RESULT_ERROR_UNSUPPORTED = -10,
    XESS_RESULT_ERROR_CANT_LOAD_LIBRARY = -11,
    XESS_RESULT_ERROR_WRONG_CALL_ORDER = -12,
    XESS_RESULT_ERROR_UNKNOWN = -1000,
} xess_result_t;

typedef struct _xess_d3d12_init_params_t xess_d3d12_init_params_t;
typedef struct _xess_d3d12_execute_params_t xess_d3d12_execute_params_t;

xess_result_t __cdecl xessGetVersion(xess_version_t* pVersion);
xess_result_t __cdecl xessGetOptimalInputResolution(
    xess_context_handle_t hContext,
    const xess_2d_t* pOutputResolution,
    xess_quality_settings_t qualitySettings,
    xess_2d_t* pInputResolutionOptimal,
    xess_2d_t* pInputResolutionMin,
    xess_2d_t* pInputResolutionMax);
xess_result_t __cdecl xessDestroyContext(xess_context_handle_t hContext);
xess_result_t __cdecl xessSetVelocityScale(xess_context_handle_t hContext, float x, float y);
xess_result_t __cdecl xessD3D12CreateContext(ID3D12Device* pDevice, xess_context_handle_t* phContext);
xess_result_t __cdecl xessD3D12Init(xess_context_handle_t hContext, const xess_d3d12_init_params_t* pInitParams);
xess_result_t __cdecl xessD3D12Execute(
    xess_context_handle_t hContext,
    ID3D12GraphicsCommandList* pCommandList,
    const xess_d3d12_execute_params_t* pExecParams);

}

static_assert(sizeof(xess_version_t) == 8);
static_assert(sizeof(xess_2d_t) == 8);
static_assert(sizeof(xess_quality_settings_t) == 4);
static_assert(sizeof(xess_result_t) == 4);

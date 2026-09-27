#pragma once

#include <cstdint>
#include <cstddef>

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

typedef xess_2d_t xess_coord_t;

typedef enum _xess_init_flags_t : int32_t {
    XESS_INIT_FLAG_NONE = 0,
    XESS_INIT_FLAG_HIGH_RES_MV = 1 << 0,
    XESS_INIT_FLAG_INVERTED_DEPTH = 1 << 1,
    XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE = 1 << 2,
    XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK = 1 << 3,
    XESS_INIT_FLAG_USE_NDC_VELOCITY = 1 << 4,
    XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1 << 5,
    XESS_INIT_FLAG_LDR_INPUT_COLOR = 1 << 6,
    XESS_INIT_FLAG_JITTERED_MV = 1 << 7,
    XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE = 1 << 8,
} xess_init_flags_t;
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

typedef struct _xess_d3d12_init_params_t {
    xess_2d_t outputResolution;
    xess_quality_settings_t qualitySetting;
    uint32_t initFlags;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    ID3D12Heap* pTempBufferHeap;
    uint64_t bufferHeapOffset;
    ID3D12Heap* pTempTextureHeap;
    uint64_t textureHeapOffset;
    ID3D12PipelineLibrary* pPipelineLibrary;
} xess_d3d12_init_params_t;

typedef struct _xess_d3d12_execute_params_t {
    ID3D12Resource* pColorTexture;
    ID3D12Resource* pVelocityTexture;
    ID3D12Resource* pDepthTexture;
    ID3D12Resource* pExposureScaleTexture;
    ID3D12Resource* pResponsivePixelMaskTexture;
    ID3D12Resource* pOutputTexture;
    float jitterOffsetX;
    float jitterOffsetY;
    float exposureScale;
    uint32_t resetHistory;
    uint32_t inputWidth;
    uint32_t inputHeight;
    xess_coord_t inputColorBase;
    xess_coord_t inputMotionVectorBase;
    xess_coord_t inputDepthBase;
    xess_coord_t inputResponsiveMaskBase;
    xess_coord_t reserved0;
    xess_coord_t outputColorBase;
    ID3D12DescriptorHeap* pDescriptorHeap;
    uint32_t descriptorHeapOffset;
} xess_d3d12_execute_params_t;
#pragma pack(pop)

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
static_assert(sizeof(xess_init_flags_t) == 4);
static_assert(sizeof(xess_d3d12_init_params_t) == 64);
static_assert(sizeof(xess_d3d12_execute_params_t) == 136);
static_assert(offsetof(xess_d3d12_execute_params_t, pOutputTexture) == 40);
static_assert(offsetof(xess_d3d12_execute_params_t, jitterOffsetX) == 48);
static_assert(offsetof(xess_d3d12_execute_params_t, inputWidth) == 64);
static_assert(offsetof(xess_d3d12_execute_params_t, inputColorBase) == 72);
static_assert(offsetof(xess_d3d12_execute_params_t, pDescriptorHeap) == 120);
static_assert(offsetof(xess_d3d12_execute_params_t, descriptorHeapOffset) == 128);

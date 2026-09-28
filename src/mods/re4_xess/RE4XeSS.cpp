#include "mods/re4_xess/RE4XeSS.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sdk/GameIdentity.hpp>
#include <sdk/RETypeDB.hpp>
#include <sdk/SceneManager.hpp>
#include <spdlog/spdlog.h>
#include <safetyhook.hpp>
#include <utility/Address.hpp>
#include <utility/Module.hpp>
#include <utility/VtableHook.hpp>

#include "mods/REFrameworkConfig.hpp"
#include "REFramework.hpp"

namespace {

using UpscalingMode = RE4XeSS::UpscalingMode;

constexpr std::string_view UPSCALING_MODE_CONFIG_KEY{ "RE4XeSS_UpscalingMode" };
constexpr char INHIBIT_BIT_BACKING_FIELD_NAME[]{ "<InhibitBit>k__BackingField" };

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

    bool arm(
        uint64_t frame_id,
        sdk::renderer::TargetState* overlay_state,
        ID3D12Resource* semantic_color) {
        if (!sdk::GameIdentity::get().is_re4() || overlay_state == nullptr || semantic_color == nullptr) {
            return false;
        }

        const auto config = REFrameworkConfig::get();
        if (config == nullptr || !config->is_debug_log_enabled()) {
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
        m_overlay_state = reinterpret_cast<uintptr_t>(overlay_state);
        m_overlay_desc = overlay_state->get_desc_base();
        m_overlay_rtv_array = reinterpret_cast<uintptr_t>(overlay_state->get_rtvs_ptr());
        m_overlay_rtv = 0;
        if (overlay_state->get_rtv_count() != 0 && overlay_state->get_rtvs_ptr() != nullptr) {
            // Read the intrusive pointer's raw value only; do not AddRef/Release it.
            m_overlay_rtv = reinterpret_cast<uintptr_t>(overlay_state->get_rtvs_ptr()[0].get());
        }
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
        log_overlay_state_snapshot(overlay_state);
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

    void on_post_present() noexcept {
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

private:
    struct ImageInfo {
        uintptr_t base{};
        uint32_t size{};
        uint32_t checksum{};
        uint32_t section_table_rva{};
        IMAGE_NT_HEADERS64 nt{};
        bool valid{};
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

    static constexpr uint32_t EXPECTED_IMAGE_SIZE = 0x0E405000;
    static constexpr uint32_t EXPECTED_IMAGE_CHECKSUM = 0x0DEE3479;
    static constexpr size_t SITE_COUNT = 4;
    // Isolate the nested owner vcall so its ABI and return can be measured without other probe hooks.
    static constexpr size_t ISOLATED_SITE_INDEX = 1;
    static constexpr uint32_t MAX_PENDING_PRESENT_GRACE = 2;
    static constexpr size_t MAX_CAPTURED_CALLS_PER_SITE = 4;
    static constexpr size_t MAX_CAPTURED_CALLS = SITE_COUNT * MAX_CAPTURED_CALLS_PER_SITE;
    static constexpr size_t HOOK_COUNT = SITE_COUNT * 2;
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
    uintptr_t m_image_base{};
    uintptr_t m_image_end{};
    uint64_t m_frame_id{};
    DWORD m_arm_thread_id{};
    uintptr_t m_overlay_state{};
    uintptr_t m_overlay_desc{};
    uintptr_t m_overlay_rtv_array{};
    uintptr_t m_overlay_rtv{};
    uintptr_t m_semantic_color{};
    std::array<safetyhook::MidHook, HOOK_COUNT> m_hooks{};
    inline static thread_local std::array<PendingObservation, MAX_CAPTURED_CALLS> s_pending_observations{};

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

    static bool matches_bytes(uintptr_t address, std::initializer_list<uint8_t> expected) noexcept {
        std::array<uint8_t, 32> bytes{};
        if (expected.size() > bytes.size() || !read_memory(address, bytes.data(), expected.size())) {
            return false;
        }
        return std::equal(expected.begin(), expected.end(), bytes.begin());
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

    std::string identity_matches(uintptr_t value) const {
        std::ostringstream text;
        text << "state=" << (value == m_overlay_state)
             << ",desc=" << (value == m_overlay_desc)
             << ",rtvArray=" << (value == m_overlay_rtv_array)
             << ",rtv0=" << (value == m_overlay_rtv)
             << ",color=" << (value == m_semantic_color);
        return text.str();
    }

    void log_overlay_state_snapshot(sdk::renderer::TargetState* state) const {
        if (state == nullptr) {
            return;
        }
        const auto& desc = state->get_desc();
        spdlog::info("[RE4XeSS][TargetStateProbe] overlaySnapshot state=0x{:x} desc=0x{:x} rtvArray=0x{:x} count={} rtv0=0x{:x} rect=({:.1f},{:.1f},{:.1f},{:.1f}) flag=0x{:x} color=0x{:x}",
            m_overlay_state,
            m_overlay_desc,
            m_overlay_rtv_array,
            state->get_rtv_count(),
            m_overlay_rtv,
            desc.rect.left,
            desc.rect.top,
            desc.rect.right,
            desc.rect.bottom,
            desc.flag,
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
        } catch (...) {
            // Probe failures must not affect the original engine call.
        }
    }

    static uintptr_t read_pointer(uintptr_t address) noexcept {
        uintptr_t value{};
        return read_memory(address, &value, sizeof(value)) ? value : 0;
    }
};

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
    TargetStateFactoryProbe::instance().disarm("RE4XeSS destroyed");
    CreateRenderTargetViewProbe::instance().reset();
    m_worker.stop();
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
    TargetStateFactoryProbe::instance().on_post_present();
    const auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr) {
        return;
    }
    m_output_handoff.on_post_present(hook->get_device(), hook->get_command_queue());
}

void RE4XeSS::on_device_reset() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }
    TargetStateFactoryProbe::instance().disarm("device reset");
    CreateRenderTargetViewProbe::instance().reset();
    m_device_reset_generation.fetch_add(1, std::memory_order_acq_rel);
}

void RE4XeSS::on_config_load(const utility::Config& cfg) {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

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
}

void RE4XeSS::request_mode(UpscalingMode mode) {
    const auto old_mode = m_requested_mode.load(std::memory_order_acquire);
    if (mode == old_mode) {
        return;
    }

    m_requested_mode.store(mode, std::memory_order_release);
    m_control_generation.fetch_add(1, std::memory_order_acq_rel);
    spdlog::info("[RE4XeSS][Config] mode changed: {} -> {}",
        mode_to_display_label(old_mode), mode_to_display_label(mode));

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
    (void)scene_view;
    if (!is_temporal_active() || !m_load_observation_valid || result == nullptr ||
        m_input_resolution.optimal.x == 0 || m_input_resolution.optimal.y == 0) {
        return;
    }

    result[0] = static_cast<float>(m_input_resolution.optimal.x);
    result[1] = static_cast<float>(m_input_resolution.optimal.y);
}

void RE4XeSS::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (!is_temporal_active() || camera == nullptr || result == nullptr) {
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

    if (!m_load_observation_valid) {
        invalidate_history("load-state-observation-unavailable");
        return;
    }
    if (m_inhibit_departure_pending || m_load_transition_active || m_startup_mid_load) {
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
}

bool RE4XeSS::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    (void)render_context;
    const auto callback_thread_id = GetCurrentThreadId();
    const auto debug_log = REFrameworkConfig::get()->is_debug_log_enabled();
    const auto overlap_epoch = m_pre_overlay_overlap_epoch.load(std::memory_order_acquire);
    if (m_pre_overlay_in_progress.test_and_set(std::memory_order_acquire)) {
        m_pre_overlay_overlap_epoch.fetch_add(1, std::memory_order_acq_rel);
        const auto overlap_count = m_pre_overlay_overlap_count.fetch_add(1, std::memory_order_relaxed) + 1;
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

    std::string restore_error;
    if (!m_output_handoff.restore(layer, restore_error)) {
        const auto handoff = m_output_handoff.snapshot();
        set_owner_unavailable(restore_error, true,
            handoff.retirement == RE4XeSSOutputHandoff::RetirementStatus::Quarantined);
        invalidate_history("output-handoff-restore-failed");
        clear_frame_state();
        return true;
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
    ID3D12Device4* rtv_probe_device{};
    if (control_request.active) {
        (void)get_display_resolution(control_request.display);
    }
    if (requested_mode == UpscalingMode::Off) {
        CreateRenderTargetViewProbe::instance().reset();
    }
    if (g_framework != nullptr && g_framework->get_renderer_type() == REFramework::RendererType::D3D12) {
        const auto& hook = g_framework->get_d3d12_hook();
        if (hook != nullptr) {
            rtv_probe_device = hook->get_device();
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
        return true;
    }

    if (!is_temporal_active()) {
        m_output_handoff.request_retirement("temporal upscaling mode is inactive or stale");
        m_output_handoff.poll_retirement(
            control_result.snapshot.bridge_idle,
            control_result.snapshot.bridge_device_removed);
        clear_frame_state();
        return true;
    }
    if (!m_load_observation_valid) {
        invalidate_history("load-state-observation-unavailable");
        clear_frame_state();
        return true;
    }
    if (m_inhibit_departure_pending || m_load_transition_active || m_startup_mid_load) {
        invalidate_history("load-history-invalid");
        clear_frame_state();
        return true;
    }
    if (layer == nullptr) {
        invalidate_history("pre-overlay-layer-unavailable");
        clear_frame_state();
        return true;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame || !m_cached_scene || !m_cached_scene_frame ||
        *m_cached_scene_frame != static_cast<uint64_t>(*frame)) {
        invalidate_history("pre-overlay-primary-scene-frame-missing");
        clear_frame_state();
        return true;
    }

    if (!m_camera_metadata_valid || !m_camera_frame || *m_camera_frame != *m_cached_scene_frame) {
        invalidate_history("pre-overlay-temporal-gate-invalid");
        clear_frame_state();
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
        return true;
    }

    const auto render_width = m_input_resolution.optimal.x;
    const auto render_height = m_input_resolution.optimal.y;
    if (!is_valid_texture_extent(color, render_width, render_height) ||
        !is_valid_texture_extent(depth, render_width, render_height) ||
        !is_valid_texture_extent(velocity, render_width, render_height)) {
        invalidate_history("temporal-resource-extent-invariant-failed");
        clear_frame_state();
        return true;
    }
    if (color->GetDesc().Format != DXGI_FORMAT_R11G11B10_FLOAT) {
        const std::string reason{ "The current RE4 Color resource is not R11G11B10_FLOAT; PR3 output contract cannot be met" };
        mark_execution_fault(reason);
        set_owner_unavailable(reason, false, true);
        invalidate_history("unsupported-color-format");
        clear_frame_state();
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
    (void)CreateRenderTargetViewProbe::instance().ensure(
        rtv_probe_device,
        packet.frame_id,
        packet.display_width,
        packet.display_height);
    auto& target_state_probe = TargetStateFactoryProbe::instance();
    (void)target_state_probe.arm(packet.frame_id, color_state, color);
    const bool handoff_prepared = m_output_handoff.prepare(
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
    target_state_probe.on_prepare_return(handoff_prepared);
    if (!handoff_prepared) {
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
            return true;
        }

        std::string install_error;
        if (!m_output_handoff.install(layer, packet.frame_id, install_error)) {
            m_output_handoff.request_retirement("XeSS output was submitted but Overlay installation failed");
            set_owner_unavailable(install_error, true, true);
            invalidate_history("output-handoff-install-failed");
            clear_frame_state();
            return true;
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
    } else if (submit_result.status == RE4XeSSWorker::SubmitResult::Status::Busy ||
        submit_result.status == RE4XeSSWorker::SubmitResult::Status::Stale ||
        submit_result.status == RE4XeSSWorker::SubmitResult::Status::NotReady) {
        invalidate_history(submit_result.status == RE4XeSSWorker::SubmitResult::Status::Busy
                ? "xess-command-ring-busy"
                : "xess-submit-stale");
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
    }

    clear_frame_state();
    return true;
}



void RE4XeSS::on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    (void)render_context;
    m_output_handoff.observe_overlay(layer);
}

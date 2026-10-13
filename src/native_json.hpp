#pragma once
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <json.hpp>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ftl {
// Check against each destination's actual numeric type before nlohmann's
// permissive conversions can truncate a fraction or wrap a stable ID.
template <typename T> void nativeRead(const nlohmann::json& json, T& value) {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!json.is_number_integer() ||
            !(json.is_number_unsigned() ? std::in_range<T>(json.get<std::uint64_t>())
                                        : std::in_range<T>(json.get<std::int64_t>()))) {
            throw std::runtime_error("invalid_checkpoint_integer");
        }
        json.get_to(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!json.is_number()) {
            throw std::runtime_error("invalid_checkpoint_number");
        }
        const auto converted = json.get<T>();
        if (!std::isfinite(converted)) {
            throw std::runtime_error("invalid_checkpoint_number");
        }
        value = converted;
    } else if constexpr (requires {
                             typename T::mapped_type;
                             typename T::key_type;
                         }) {
        if constexpr (std::is_same_v<typename T::key_type, std::string>) {
            if (!json.is_object()) {
                throw std::runtime_error("invalid_checkpoint_map");
            }
            for (const auto& row : json) {
                typename T::mapped_type element{};
                nativeRead(row, element);
            }
        } else {
            if (!json.is_array()) {
                throw std::runtime_error("invalid_checkpoint_map");
            }
            for (const auto& row : json) {
                if (!row.is_array() || row.size() != 2) {
                    throw std::runtime_error("invalid_checkpoint_map_entry");
                }
                typename T::key_type key{};
                typename T::mapped_type element{};
                nativeRead(row.at(0), key);
                nativeRead(row.at(1), element);
            }
        }
        json.get_to(value);
    } else if constexpr (!std::is_same_v<T, nlohmann::json> && !std::is_same_v<T, std::string> &&
                         requires {
                             typename T::value_type;
                             value.begin();
                         }) {
        if (!json.is_array()) {
            throw std::runtime_error("invalid_checkpoint_array");
        }
        if constexpr (requires { std::tuple_size<T>::value; }) {
            if (json.size() != std::tuple_size_v<T>) {
                throw std::runtime_error("invalid_checkpoint_array_length");
            }
        }
        for (const auto& row : json) {
            typename T::value_type element{};
            nativeRead(row, element);
        }
        json.get_to(value);
    } else if constexpr (requires {
                             typename T::first_type;
                             typename T::second_type;
                         }) {
        if (!json.is_array() || json.size() != 2) {
            throw std::runtime_error("invalid_checkpoint_pair");
        }
        nativeRead(json.at(0), value.first);
        nativeRead(json.at(1), value.second);
    } else {
        json.get_to(value);
    }
}

template <typename T> T nativeValue(const nlohmann::json& json) {
    T value{};
    nativeRead(json, value);
    return value;
}
} // namespace ftl

// Explicit native macros leave the third-party JSON macros untouched.
#define FTL_NATIVE_JSON_FROM(member)                                                               \
    ftl::nativeRead(nlohmann_json_j.at(#member), nlohmann_json_t.member);
#define FTL_NATIVE_JSON_FROM_DEFAULT(member)                                                       \
    if (!nlohmann_json_j.is_null() && nlohmann_json_j.contains(#member)) {                         \
        ftl::nativeRead(nlohmann_json_j.at(#member), nlohmann_json_t.member);                      \
    } else {                                                                                       \
        nlohmann_json_t.member = nlohmann_json_default_obj.member;                                 \
    }
#define FTL_NATIVE_JSON(Type, ...)                                                                 \
    friend void to_json(nlohmann::json& nlohmann_json_j, const Type& nlohmann_json_t) {            \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__))                   \
    }                                                                                              \
    friend void from_json(const nlohmann::json& nlohmann_json_j, Type& nlohmann_json_t) {          \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM, __VA_ARGS__))               \
    }
#define FTL_NATIVE_JSON_DEFAULT(Type, ...)                                                         \
    friend void to_json(nlohmann::json& nlohmann_json_j, const Type& nlohmann_json_t) {            \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__))                   \
    }                                                                                              \
    friend void from_json(const nlohmann::json& nlohmann_json_j, Type& nlohmann_json_t) {          \
        const Type nlohmann_json_default_obj{};                                                    \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, __VA_ARGS__))       \
    }

inline void to_json(nlohmann::json& json, const SDL_FPoint& point) {
    json = nlohmann::json::array({point.x, point.y});
}

inline void from_json(const nlohmann::json& json, SDL_FPoint& point) {
    if (!json.is_array() || json.size() != 2) {
        throw std::runtime_error("invalid_checkpoint_point");
    }
    ftl::nativeRead(json.at(0), point.x);
    ftl::nativeRead(json.at(1), point.y);
}

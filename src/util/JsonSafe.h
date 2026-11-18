#pragma once

// Type-checked reads of JSON that somebody else wrote.
//
// nlohmann's `value(key, default)` and `get<T>()` THROW type_error when the
// field holds the wrong type — `value("formatted_body", "")` on
// `"formatted_body": 123`, or `value("video_rtp", 0)` on `"video_rtp": "1"`.
// Event content, call signalling and peer datachannel messages are written by
// other users' clients, so a throwing read there is a remote crash: the
// exception unwinds out of a slot or a Q_INVOKABLE and terminates the app, and
// for a timeline event it does so again on every launch, from the cache.
//
// Everything here returns the default instead: wrong type, missing key, or a
// container that is not an object at all. Use these for ANY read of
// sender-controlled JSON.

#include <QString>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace bsfchat::jsonsafe {

// The member `key` of `obj`, or nullptr when `obj` is not an object or has
// no such key.
inline const nlohmann::json* member(const nlohmann::json& obj, const char* key)
{
    if (!obj.is_object()) return nullptr;
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

inline std::string str(const nlohmann::json& obj, const char* key,
                       const std::string& def = std::string())
{
    const auto* v = member(obj, key);
    return v && v->is_string() ? v->get_ref<const std::string&>() : def;
}

inline QString qstr(const nlohmann::json& obj, const char* key)
{
    const auto* v = member(obj, key);
    return v && v->is_string()
        ? QString::fromStdString(v->get_ref<const std::string&>()) : QString();
}

// Any JSON number, clamped into [lo, hi]. Floats are truncated; NaN and
// non-numbers give `def`.
inline std::int64_t integer(const nlohmann::json& obj, const char* key,
                            std::int64_t def = 0,
                            std::int64_t lo = std::numeric_limits<std::int64_t>::min(),
                            std::int64_t hi = std::numeric_limits<std::int64_t>::max())
{
    const auto* v = member(obj, key);
    if (!v) return def;
    if (v->is_number_unsigned()) {
        const auto u = v->get<std::uint64_t>();
        return u > static_cast<std::uint64_t>(hi) ? hi
             : static_cast<std::int64_t>(u) < lo  ? lo
                                                   : static_cast<std::int64_t>(u);
    }
    if (v->is_number_integer()) {
        const auto i = v->get<std::int64_t>();
        return i < lo ? lo : i > hi ? hi : i;
    }
    if (v->is_number_float()) {
        const double d = v->get<double>();
        if (std::isnan(d)) return def;
        if (d <= static_cast<double>(lo)) return lo;
        if (d >= static_cast<double>(hi)) return hi;
        return static_cast<std::int64_t>(d);
    }
    return def;
}

// integer() narrowed to int.
inline int int32(const nlohmann::json& obj, const char* key, int def = 0)
{
    return static_cast<int>(integer(obj, key, def, std::numeric_limits<int>::min(),
                                    std::numeric_limits<int>::max()));
}

// A non-negative count. Negative input clamps to 0.
inline std::uint64_t count(const nlohmann::json& obj, const char* key)
{
    return static_cast<std::uint64_t>(integer(obj, key, 0, 0));
}

// A boolean, also accepting the 0/1 numbers some senders' libraries write.
inline bool boolean(const nlohmann::json& obj, const char* key, bool def = false)
{
    const auto* v = member(obj, key);
    if (!v) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0.0;
    return def;
}

// The member as an object/array, or a shared empty one — never a throw, and
// safe to iterate or to pass back into these helpers.
inline const nlohmann::json& object(const nlohmann::json& obj, const char* key)
{
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const auto* v = member(obj, key);
    return v && v->is_object() ? *v : kEmpty;
}

inline const nlohmann::json& array(const nlohmann::json& obj, const char* key)
{
    static const nlohmann::json kEmpty = nlohmann::json::array();
    const auto* v = member(obj, key);
    return v && v->is_array() ? *v : kEmpty;
}

} // namespace bsfchat::jsonsafe

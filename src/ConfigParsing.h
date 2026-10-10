// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace vt::config::parsing
{
constexpr bool Space(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}
constexpr std::string_view Trim(std::string_view value) noexcept
{
    while (!value.empty() && Space(value.front())) value.remove_prefix(1);
    while (!value.empty() && Space(value.back())) value.remove_suffix(1);
    return value;
}
constexpr bool EqualIgnoreCase(std::string_view value, std::string_view token) noexcept
{
    if (value.size() != token.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const auto ch = value[i] >= 'A' && value[i] <= 'Z' ? value[i] + ('a' - 'A') : value[i];
        if (ch != token[i]) return false;
    }
    return true;
}
[[nodiscard]] constexpr std::optional<bool> Boolean(std::string_view value) noexcept
{
    value = Trim(value);
    if (EqualIgnoreCase(value, "true") || EqualIgnoreCase(value, "yes") || EqualIgnoreCase(value, "on") || value == "1")
        return true;
    if (EqualIgnoreCase(value, "false") || EqualIgnoreCase(value, "no") || EqualIgnoreCase(value, "off") || value == "0")
        return false;
    return std::nullopt;
}
[[nodiscard]] inline std::optional<std::int64_t> Integer(std::string_view value) noexcept
{
    value = Trim(value);
    if (value.empty()) return std::nullopt;
    const bool negative = value.front() == '-';
    if (negative || value.front() == '+') value.remove_prefix(1);
    int base = 10;
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    { value.remove_prefix(2); base = 16; }
    if (value.empty()) return std::nullopt;
    std::uint64_t magnitude = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), magnitude, base);
    constexpr auto signedMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto limit = signedMax + (negative ? 1u : 0u);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || magnitude > limit)
        return std::nullopt;
    // INT64_MIN is valid; avoid negating or casting its unsigned magnitude.
    if (negative && magnitude) return -static_cast<std::int64_t>(magnitude - 1) - 1;
    return static_cast<std::int64_t>(magnitude);
}
[[nodiscard]] inline std::optional<double> Real(std::string_view value) noexcept
{
    value = Trim(value);
    if (value.empty()) return std::nullopt;
    if (value.front() == '+')
    {
        value.remove_prefix(1);
        if (value.empty() || value.front() == '+' || value.front() == '-') return std::nullopt;
    }
    double result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result))
        return std::nullopt;
    return result;
}

// Bounds/defaults are part of the option's type, checked before a build can
// succeed. Only a validated, bounded signed value is cast to its storage type.
template <class Value, std::int64_t Default, std::int64_t Minimum, std::int64_t Maximum>
struct IntegerOption
{
    static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>);
    static_assert(std::numeric_limits<Value>::digits <= std::numeric_limits<std::int64_t>::digits);
    static_assert(Minimum <= Default && Default <= Maximum);
    static_assert(Minimum >= static_cast<std::int64_t>(std::numeric_limits<Value>::lowest()) &&
                  Maximum <= static_cast<std::int64_t>(std::numeric_limits<Value>::max()));
    const char *key;
    static constexpr Value defaultValue = static_cast<Value>(Default);
    [[nodiscard]] Value Parse(std::string_view text) const noexcept
    {
        const auto parsed = Integer(text);
        return parsed ? static_cast<Value>(std::clamp(*parsed, Minimum, Maximum)) : defaultValue;
    }
};
struct RealOption
{
    const char *key;
    double defaultValue, minimum, maximum;
    constexpr bool Valid() const noexcept { return minimum <= defaultValue && defaultValue <= maximum; }
    [[nodiscard]] double Parse(std::string_view text) const noexcept
    {
        const auto parsed = Real(text);
        return parsed ? std::clamp(*parsed, minimum, maximum) : defaultValue;
    }
};
// The single source of defaults and bounds for cached numeric configuration.
inline constexpr auto intMin = std::numeric_limits<int>::min();
inline constexpr auto intMax = std::numeric_limits<int>::max();
inline constexpr IntegerOption<int, 4000, 16, intMax> maxUnique{"MaxUniqueStrings"};
inline constexpr IntegerOption<unsigned long, 2000, 250, intMax> flushMs{"FlushIntervalMs"};
inline constexpr IntegerOption<int, 400, intMin, intMax> weight{"FontWeight"};
inline constexpr IntegerOption<int, 400, intMin, intMax> latinWeight{"FontWeightLatin"};
inline constexpr IntegerOption<int, 400, intMin, intMax> symbolWeight{"FontWeightSymbol"};
inline constexpr IntegerOption<int, 13, 6, intMax> latinSize{"FontSizeLatin"};
inline constexpr IntegerOption<int, 13, 6, intMax> symbolSize{"FontSizeSymbol"};
inline constexpr IntegerOption<int, 16, 6, intMax> fontSize{"FontSize"};
inline constexpr IntegerOption<int, 13, 1, 32> baseline{"BaselineRow"};
inline constexpr IntegerOption<int, 0, intMin, intMax> darkening{"StemDarkening"};
inline constexpr IntegerOption<int, 2, 1, 4> supersample{"Supersample"};
inline constexpr IntegerOption<int, 0, 0, 2> outline{"Outline"};
inline constexpr IntegerOption<unsigned short, 0, 0, 0xFFFF> outlineColor{"OutlineColor"};
inline constexpr RealOption advanceScale{"AdvanceScale", 1.05, 1.0, 2.0};
inline constexpr RealOption gamma{"Gamma", 1.0, 0.5, 3.0};
static_assert(advanceScale.Valid() && gamma.Valid());
} // namespace vt::config::parsing

#pragma once

#include <SpectrumTransport.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

static constexpr size_t OBSERVATION_ID_MAX_LENGTH = 64;

inline bool observation_id_is_valid(const std::string &id)
{
    if (id.empty() || id.size() > OBSERVATION_ID_MAX_LENGTH)
        return false;

    for (const unsigned char c : id)
    {
        const bool ascii_alnum =
            (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z');
        if (!ascii_alnum && c != '-' && c != '_' && c != '.')
            return false;
    }
    return id != "." && id != "..";
}

inline uint32_t observation_id_numeric(const std::string &id)
{
    return spectrum_crc32c(id.data(), id.size());
}

inline std::string observation_directory_component(const std::string &value)
{
    std::string result;
    bool previous_separator = false;
    for (const unsigned char c : value)
    {
        const bool ascii_alnum =
            (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z');
        if (ascii_alnum || c == '.' || c == '_' || c == '-')
        {
            result.push_back(static_cast<char>(c));
            previous_separator = false;
        }
        else if (!result.empty() && !previous_separator)
        {
            result.push_back('-');
            previous_separator = true;
        }
    }
    while (!result.empty() && result.back() == '-')
        result.pop_back();
    return result.empty() ? "UNKNOWN" : result;
}

inline std::string automatic_observation_directory_id(
    const std::string &object = {}, const std::string &state = {})
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%d_%H-%M-%S")
           << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count() << 'Z';

    const std::string timestamp = result.str();
    const size_t prefix_capacity = OBSERVATION_ID_MAX_LENGTH - timestamp.size() - 1;
    // Reserve the complete timestamp and state even when the source is long.
    std::string safe_state = state.empty() ? "" : observation_directory_component(state);
    safe_state.resize(std::min(safe_state.size(),
                              prefix_capacity - (object.empty() ? 0 : 2)));
    std::string prefix;
    if (!object.empty()) {
        const size_t available = prefix_capacity -
            (safe_state.empty() ? 0 : safe_state.size() + 1);
        prefix = observation_directory_component(object).substr(0, available);
    }
    if (!safe_state.empty()) {
        if (!prefix.empty()) prefix.push_back('_');
        prefix += safe_state;
    }
    return prefix.empty() ? timestamp : prefix + '_' + timestamp;
}

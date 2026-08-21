#include "core/semver.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <vector>

namespace dsh::version {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(separator, start);
        parts.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

bool parse_number(std::string_view value, std::int64_t& result) {
    if (value.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() && result >= 0;
}

int compare_identifiers(const std::string& left, const std::string& right) {
    const auto left_parts = split(left, '.');
    const auto right_parts = split(right, '.');
    const auto length = std::max(left_parts.size(), right_parts.size());
    for (std::size_t index = 0; index < length; ++index) {
        if (index >= left_parts.size()) return -1;
        if (index >= right_parts.size()) return 1;
        std::int64_t left_number{};
        std::int64_t right_number{};
        const bool left_numeric = parse_number(left_parts[index], left_number);
        const bool right_numeric = parse_number(right_parts[index], right_number);
        if (left_numeric && right_numeric && left_number != right_number) {
            return left_number > right_number ? 1 : -1;
        }
        if (left_numeric != right_numeric) {
            return left_numeric ? -1 : 1;
        }
        if (left_parts[index] != right_parts[index]) {
            return left_parts[index] > right_parts[index] ? 1 : -1;
        }
    }
    return 0;
}

}  // namespace

std::optional<SemVer> parse(const std::string& raw_value) {
    auto value = trim(raw_value);
    if (!value.empty() && value.front() == 'v') {
        value.erase(value.begin());
    }
    if (const auto build = value.find('+'); build != std::string::npos) {
        value.resize(build);
    }
    std::string prerelease;
    if (const auto dash = value.find('-'); dash != std::string::npos) {
        prerelease = value.substr(dash + 1);
        value.resize(dash);
    }
    const auto parts = split(value, '.');
    if (parts.size() != 3) {
        return std::nullopt;
    }
    SemVer result;
    if (!parse_number(parts[0], result.major) || !parse_number(parts[1], result.minor) ||
        !parse_number(parts[2], result.patch)) {
        return std::nullopt;
    }
    result.prerelease = std::move(prerelease);
    return result;
}

bool is_newer(const std::string& candidate, const std::string& current) {
    const auto next = parse(candidate);
    const auto now = parse(current);
    if (!next || !now) {
        return trim(candidate) != trim(current);
    }
    if (next->major != now->major) return next->major > now->major;
    if (next->minor != now->minor) return next->minor > now->minor;
    if (next->patch != now->patch) return next->patch > now->patch;
    if (next->prerelease.empty() != now->prerelease.empty()) {
        return next->prerelease.empty();
    }
    return compare_identifiers(next->prerelease, now->prerelease) > 0;
}

}  // namespace dsh::version


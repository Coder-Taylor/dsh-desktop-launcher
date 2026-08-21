#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dsh::version {

struct SemVer {
    std::int64_t major{};
    std::int64_t minor{};
    std::int64_t patch{};
    std::string prerelease;
};

std::optional<SemVer> parse(const std::string& value);
bool is_newer(const std::string& candidate, const std::string& current);

}  // namespace dsh::version


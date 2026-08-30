#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace dsh {

class Log {
public:
    explicit Log(const std::filesystem::path& state_directory);

    void info(const std::string& message);
    void error(const std::string& message);
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::filesystem::path& fallback_path() const noexcept;
    // The file this process actually appends to: the primary log unless its
    // append failed at least once, in which case the session log.
    [[nodiscard]] const std::filesystem::path& used_path() const noexcept;

private:
    void write(const char* level, const std::string& message);
    void rotate_if_needed();
    void cleanup_old_logs();

    std::filesystem::path path_;
    std::filesystem::path fallback_path_;
    bool primary_usable_{true};
    std::mutex mutex_;
};

}  // namespace dsh

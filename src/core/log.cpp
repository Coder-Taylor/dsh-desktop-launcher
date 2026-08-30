#include "core/log.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dsh {
namespace {
constexpr std::uintmax_t max_log_size = 512 * 1024;
constexpr int log_copies = 3;

std::string process_suffix() {
#ifdef _WIN32
    return std::to_string(GetCurrentProcessId());
#else
    return std::to_string(getpid());
#endif
}
}  // namespace

Log::Log(const std::filesystem::path& state_directory)
    : path_(state_directory / "logs" / "launcher.log") {
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    error.clear();
    auto temporary = std::filesystem::temp_directory_path(error);
    if (error || temporary.empty()) temporary = std::filesystem::current_path(error);
    fallback_path_ = temporary / "DshLauncher" / "logs" /
                     ("launcher-" + process_suffix() + ".log");
    std::filesystem::create_directories(fallback_path_.parent_path(), error);
    cleanup_old_logs();
}

void Log::info(const std::string& message) { write("INFO", message); }
void Log::error(const std::string& message) { write("ERROR", message); }
const std::filesystem::path& Log::path() const noexcept { return path_; }
const std::filesystem::path& Log::fallback_path() const noexcept { return fallback_path_; }

void Log::write(const char* level, const std::string& original_message) {
    std::lock_guard lock(mutex_);
    rotate_if_needed();
    auto message = original_message;
    if (message.size() > 1000) {
        message.resize(1000);
        message += "...";
    }
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream line;
    line << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << ' ' << std::left
         << std::setw(5) << level << ' ' << message << '\n';
    const auto append = [&line](const std::filesystem::path& target) {
        std::ofstream stream(target, std::ios::app);
        if (!stream) return false;
        stream << line.str();
        stream.flush();
        return stream.good();
    };
    // A locked or redirected %LOCALAPPDATA% must not make diagnostics disappear.
    // Keep a second copy under %TEMP% so startup/update failures remain inspectable.
    if (!append(path_)) append(fallback_path_);
}

void Log::rotate_if_needed() {
    std::error_code error;
    if (!std::filesystem::exists(path_, error) ||
        std::filesystem::file_size(path_, error) < max_log_size) {
        return;
    }
    for (int index = log_copies; index >= 1; --index) {
        const auto source = index == 1 ? path_ : std::filesystem::path(path_.string() + "." + std::to_string(index - 1));
        const auto target = std::filesystem::path(path_.string() + "." + std::to_string(index));
        std::filesystem::remove(target, error);
        error.clear();
        if (std::filesystem::exists(source, error)) {
            std::filesystem::rename(source, target, error);
        }
        error.clear();
    }
}

void Log::cleanup_old_logs() {
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 30);
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(path_.parent_path(), error)) {
        if (entry.is_regular_file(error) && entry.last_write_time(error) < cutoff) {
            std::filesystem::remove(entry.path(), error);
        }
        error.clear();
    }
}

}  // namespace dsh

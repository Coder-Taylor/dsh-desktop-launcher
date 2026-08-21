#pragma once

#include "core/log.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace dsh {

struct Status {
    bool installed{};
    bool running{};
    std::string executable;
    std::string version;
    std::optional<std::uint32_t> pid;
};

class Service {
public:
    using Progress = std::function<void(const std::string&)>;

    Service();

    Status detect();
    bool start(std::string& error);
    bool stop(std::string& error);
    bool open_web(std::string& error);
    bool ensure_installed(const Progress& progress, std::string& error);
    std::optional<std::string> latest_version();
    std::string node_version();
    std::string npm_version();
    [[nodiscard]] const std::filesystem::path& log_path() const noexcept;
    [[nodiscard]] std::filesystem::path service_log_path() const;

private:
    std::optional<std::uint32_t> read_pid() const;
    bool write_pid(std::uint32_t pid, std::string& error) const;
    void clear_pid() const;

    std::filesystem::path state_directory_;
    std::filesystem::path pid_file_;
    Log log_;
};

}  // namespace dsh

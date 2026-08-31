#pragma once

#include "core/log.h"
#include "platform/platform.h"

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace dsh {

struct Status {
    bool installed{};
    bool installation_incomplete{};
    bool running{};
    std::string executable;
    std::string version;
    std::string integrity_problem;
    std::optional<std::uint32_t> pid;
};

enum class InstallSource {
    mirror,
    official,
};

enum class CloseAction {
    ask,
    tray,
    exit,
};

struct EnvironmentStatus {
    bool has_node{};
    bool has_npm{};
    std::string node_version;
    std::string npm_version;
};

// Options used only by the first-install wizard.  Existing installations keep
// their recorded Node.js runtime and do not get rewritten by this flow.
struct NodeInstallOptions {
    bool install_system_node{};
    InstallSource source{InstallSource::mirror};
    std::filesystem::path directory;
};

class Service {
public:
    using Progress = std::function<void(const std::string&)>;

    Service();

    Status detect();
    bool is_running() const;
    bool start(std::string& error);
    bool stop(std::string& error);
    bool open_web(std::string& error);
    bool ensure_installed(const Progress& progress, std::string& error);
    bool install_at(const std::filesystem::path& directory, InstallSource source,
                    const Progress& progress, std::string& error,
                    const std::atomic_bool* cancel = nullptr,
                    const NodeInstallOptions* node_options = nullptr);
    bool uninstall(bool remove_dsh, bool remove_node, bool preserve_conversation_memory,
                   const Progress& progress, std::string& error);
    bool update(const Progress& progress, std::string& error,
                const std::atomic_bool* cancel = nullptr);
    bool update_launcher(const platform::LauncherUpdate& update, const Progress& progress,
                         std::string& error, const std::atomic_bool* cancel = nullptr);
    EnvironmentStatus environment();
    bool managed_node_installed() const;
    bool launcher_owned_node_installed() const;
    [[nodiscard]] std::filesystem::path default_dsh_directory() const;
    [[nodiscard]] InstallSource install_source() const noexcept;
    bool set_install_source(InstallSource source, std::string& error);
    [[nodiscard]] bool minimize_to_tray() const noexcept;
    bool set_minimize_to_tray(bool enabled, std::string& error);
    [[nodiscard]] CloseAction close_action() const noexcept;
    bool set_close_action(CloseAction action, std::string& error);
    std::optional<std::string> latest_version(const std::atomic_bool* cancel = nullptr);
    std::optional<platform::LauncherUpdate> latest_launcher_update(
        const std::atomic_bool* cancel = nullptr);
    std::string node_version();
    std::string npm_version();
    [[nodiscard]] const std::filesystem::path& log_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& fallback_log_path() const noexcept;
    // The log file this process actually appends to (primary or session log).
    [[nodiscard]] std::filesystem::path used_log_path() const;
    [[nodiscard]] std::filesystem::path service_log_path() const;

private:
    bool write_settings(std::string& error) const;
    std::optional<std::uint32_t> read_pid() const;
    bool write_pid(std::uint32_t pid, std::string& error) const;
    void clear_pid() const;

    std::filesystem::path state_directory_;
    std::filesystem::path pid_file_;
    std::filesystem::path settings_file_;
    Log log_;
    InstallSource install_source_{InstallSource::mirror};
    bool minimize_to_tray_{};
    CloseAction close_action_{CloseAction::ask};
};

}  // namespace dsh

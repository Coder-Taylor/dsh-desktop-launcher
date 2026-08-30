#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace dsh::platform {

struct LauncherUpdate {
    std::string version;
    std::string url;
    std::string fallback_url;
    std::uint64_t size{};
    std::string sha256;
};

std::filesystem::path state_directory();
std::optional<std::string> find_dsh();
// Returns the prefix saved by the launcher even if a cancelled older launcher
// left its dsh shim missing.  Callers use it only as a repair destination.
std::optional<std::filesystem::path> remembered_dsh_directory();
std::string dsh_version(const std::string& executable);
// Returns a version only after checking both the launch shim and the DSH core
// package.  An interrupted install is deliberately reported as incomplete.
std::optional<std::string> verified_dsh_version(const std::string& executable);
std::optional<std::string> latest_dsh_version(bool official_source = false,
                                              const std::atomic_bool* cancel = nullptr,
                                              std::string* diagnostic = nullptr);
std::optional<LauncherUpdate> latest_launcher_update(
    bool official_source = false,
    const std::atomic_bool* cancel = nullptr,
    std::string* diagnostic = nullptr);
bool update_launcher(const LauncherUpdate& update, std::string& error,
                     const std::atomic_bool* cancel = nullptr);
bool has_node();
bool has_npm();
std::string node_version();
std::string npm_version();
std::filesystem::path default_dsh_directory();
bool install_managed_node(bool official_source, std::string& error,
                          const std::atomic_bool* cancel = nullptr);
bool install_managed_node(std::string& error);
// Installs a normal Windows Node.js LTS runtime (the same MSI/winget routes as
// the legacy BAT).  `directory` is the selected Node.js installation folder.
// The MSI path asks for UAC through ShellExecute; it never silently elevates.
bool install_system_node(bool official_source, const std::filesystem::path& directory,
                         std::string& error, const std::atomic_bool* cancel = nullptr);
bool install_dsh_at(const std::filesystem::path& directory, bool official_source, std::string& error,
                    const std::string& requested_version = {},
                    const std::atomic_bool* cancel = nullptr,
                    const std::function<void(const std::string&)>& progress = {});
bool install_managed_dsh(std::string& error);
bool uninstall_dsh(std::string& error);
// Deletes only DSH session/local-storage directories, never credentials,
// settings, profiles, logs, or the DSH_HOME root itself.
bool clear_conversation_memory(std::string& error);
bool has_managed_node();
bool has_launcher_owned_node();
bool uninstall_launcher_owned_node(std::string& error);
bool uninstall_managed_node(std::string& error);
bool is_web_running();
std::optional<std::uint32_t> start_dsh(
    const std::string& executable,
    const std::filesystem::path& service_log,
    std::string& error);
bool stop_process_tree(std::uint32_t pid, std::string& error);
bool open_web(std::string& error);

}  // namespace dsh::platform

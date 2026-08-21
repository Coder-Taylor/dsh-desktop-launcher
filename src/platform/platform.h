#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace dsh::platform {

std::filesystem::path state_directory();
std::optional<std::string> find_dsh();
std::string dsh_version(const std::string& executable);
std::optional<std::string> latest_dsh_version();
bool has_node();
bool has_npm();
std::string node_version();
std::string npm_version();
bool install_managed_node(std::string& error);
bool install_managed_dsh(std::string& error);
bool is_web_running();
std::optional<std::uint32_t> start_dsh(
    const std::string& executable,
    const std::filesystem::path& service_log,
    std::string& error);
bool stop_process_tree(std::uint32_t pid, std::string& error);
bool open_web(std::string& error);

}  // namespace dsh::platform

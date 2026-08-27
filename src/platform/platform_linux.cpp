#ifndef _WIN32

#include "platform/platform.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <regex>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>

namespace dsh::platform {
namespace {
constexpr std::array<const char*, 3> domestic_npm_registries{
    "https://registry.npmmirror.com",
    "https://mirrors.cloud.tencent.com/npm",
    "https://repo.huaweicloud.com/repository/npm"};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string capture(const std::string& command) {
    std::array<char, 512> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
    pclose(pipe);
    return trim(output);
}
std::optional<std::string> extract_version(const std::string& output) {
    static const std::regex pattern(R"((?:^|\s)v?(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)(?:\s|$))");
    std::smatch match;
    if (std::regex_search(output, match, pattern)) return match[1].str();
    return std::nullopt;
}
}  // namespace

std::filesystem::path state_directory() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) {
        return std::filesystem::path(state) / "dsh-launcher";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "state" / "dsh-launcher";
    }
    return std::filesystem::temp_directory_path() / "dsh-launcher";
}

std::optional<std::string> find_dsh() {
    const auto path = capture("command -v dsh 2>/dev/null");
    return path.empty() ? std::nullopt : std::optional<std::string>(path);
}
std::string dsh_version(const std::string& executable) { return capture("\"" + executable + "\" --version 2>/dev/null"); }
std::optional<std::string> latest_dsh_version(bool official_source, const std::atomic_bool*,
                                              std::string* diagnostic) {
    const auto query = [diagnostic](const std::string& registry) {
        const auto output = capture("npm view @deepseek-ai/dsh version --registry=" + registry + " 2>&1");
        const auto version = extract_version(output);
        if (!version && diagnostic) {
            if (!diagnostic->empty()) *diagnostic += "; ";
            *diagnostic += registry + " => " + (output.empty() ? "无输出" : output);
        }
        return version;
    };
    if (official_source) {
        return query("https://registry.npmjs.org");
    }
    for (const auto* registry : domestic_npm_registries) {
        auto version = query(registry);
        if (version) return version;
    }
    return query("https://registry.npmjs.org");
}
std::optional<LauncherUpdate> latest_launcher_update(bool, const std::atomic_bool*,
                                                     std::string* diagnostic) {
    if (diagnostic) *diagnostic = "Linux 启动器更新清单尚未接入。";
    return std::nullopt;
}
bool update_launcher(const LauncherUpdate&, std::string& error, const std::atomic_bool*) {
    error = "Linux 启动器更新尚未接入。";
    return false;
}
bool has_node() { return !capture("command -v node 2>/dev/null").empty(); }
bool has_npm() { return !capture("command -v npm 2>/dev/null").empty(); }
std::string node_version() { return capture("node --version 2>/dev/null"); }
std::string npm_version() { return capture("npm --version 2>/dev/null"); }
std::filesystem::path default_dsh_directory() { return state_directory() / "runtime" / "dsh"; }
bool install_managed_node(bool, std::string& error, const std::atomic_bool*) { return install_managed_node(error); }
bool install_managed_node(std::string& error) {
    error = "Linux 免 root Node.js 安装将在 Linux 打包阶段接入。";
    return false;
}
bool install_system_node(bool, const std::filesystem::path&, std::string& error, const std::atomic_bool*) {
    error = "Linux 版 Node.js 系统安装流程尚未实现。";
    return false;
}
bool install_dsh_at(const std::filesystem::path&, bool, std::string& error, const std::string&,
                    const std::atomic_bool*, const std::function<void(const std::string&)>&) {
    return install_managed_dsh(error);
}
bool install_managed_dsh(std::string& error) {
    error = "Linux DSH 托管安装将在 Linux 打包阶段接入。";
    return false;
}
bool uninstall_dsh(std::string& error) { error = "Linux 卸载流程尚未实现。"; return false; }
bool clear_conversation_memory(std::string& error) {
    error = "Linux 对话记忆清理流程将在 Linux 打包阶段接入。";
    return false;
}
bool has_managed_node() { return false; }
bool has_launcher_owned_node() { return false; }
bool uninstall_launcher_owned_node(std::string& error) {
    error = "Linux 启动器管理的 Node.js 卸载流程尚未实现。";
    return false;
}
bool uninstall_managed_node(std::string& error) { error = "Linux 托管 Node.js 卸载流程尚未实现。"; return false; }
bool is_web_running() {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(3080);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const bool connected = connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    close(descriptor);
    return connected;
}
std::optional<std::uint32_t> start_dsh(const std::string& executable, const std::filesystem::path& service_log, std::string& error) {
    std::filesystem::create_directories(service_log.parent_path());
    const pid_t pid = fork();
    if (pid < 0) {
        error = "fork 失败。";
        return std::nullopt;
    }
    if (pid == 0) {
        setsid();
        const int log_fd = open(service_log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        execl(executable.c_str(), executable.c_str(), "web", "--no-open", static_cast<char*>(nullptr));
        _exit(127);
    }
    return static_cast<std::uint32_t>(pid);
}
bool stop_process_tree(std::uint32_t pid, std::string& error) {
    if (kill(-static_cast<pid_t>(pid), SIGTERM) != 0 && kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        error = "无法停止 DSH 进程。";
        return false;
    }
    return true;
}
bool open_web(std::string& error) {
    if (std::system("xdg-open http://127.0.0.1:3080 >/dev/null 2>&1 &") != 0) {
        error = "无法打开系统默认浏览器。";
        return false;
    }
    return true;
}
}  // namespace dsh::platform

#endif

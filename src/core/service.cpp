#include "core/service.h"

#include "platform/platform.h"

#include <fstream>

namespace dsh {

Service::Service()
    : state_directory_(platform::state_directory()),
      pid_file_(state_directory_ / "dsh-web.pid"),
      log_(state_directory_) {
    std::filesystem::create_directories(state_directory_);
    log_.info("Launcher service initialized; version=0.1.0-dev");
}

Status Service::detect() {
    Status status;
    status.running = platform::is_web_running();
    status.pid = read_pid();
    if (const auto executable = platform::find_dsh()) {
        status.installed = true;
        status.executable = *executable;
        status.version = platform::dsh_version(*executable);
    }
    return status;
}

bool Service::start(std::string& error) {
    if (platform::is_web_running()) {
        return true;
    }
    const auto executable = platform::find_dsh();
    if (!executable) {
        error = "未检测到 dsh，请先安装 DeepSeek Harness。";
        return false;
    }
    const auto service_log = state_directory_ / "logs" / "dsh-web.log";
    const auto pid = platform::start_dsh(*executable, service_log, error);
    if (!pid) {
        log_.error("Failed to start DSH: " + error);
        return false;
    }
    if (!write_pid(*pid, error)) {
        std::string ignored;
        platform::stop_process_tree(*pid, ignored);
        return false;
    }
    log_.info("DSH started; pid=" + std::to_string(*pid));
    return true;
}

bool Service::stop(std::string& error) {
    const auto pid = read_pid();
    if (!pid) {
        if (platform::is_web_running()) {
            error = "3080 端口正在使用，但进程不是由本启动器启动；为了避免误杀，已拒绝停止。";
            return false;
        }
        return true;
    }
    if (!platform::stop_process_tree(*pid, error)) {
        log_.error("Failed to stop DSH: " + error);
        return false;
    }
    clear_pid();
    log_.info("DSH stopped; pid=" + std::to_string(*pid));
    return true;
}

bool Service::open_web(std::string& error) { return platform::open_web(error); }

bool Service::ensure_installed(const Progress& progress, std::string& error) {
    if (!platform::has_node() || !platform::has_npm()) {
        progress("未检测到完整的 Node.js 环境，正在安装轻量运行环境");
        log_.info("Installing managed Node.js runtime");
        if (!platform::install_managed_node(error)) {
            log_.error("Managed Node.js installation failed: " + error);
            return false;
        }
        progress("Node.js 和 npm 已安装");
    } else {
        progress("Node.js " + platform::node_version() + "，npm " + platform::npm_version());
    }

    if (platform::find_dsh()) {
        progress("已找到本机 DSH");
        return true;
    }

    progress("正在从国内镜像下载 DSH");
    log_.info("Installing managed DSH runtime");
    if (!platform::install_managed_dsh(error)) {
        log_.error("Managed DSH installation failed: " + error);
        return false;
    }
    if (!platform::find_dsh()) {
        error = "安装命令已完成，但未找到 DSH 可执行文件。";
        return false;
    }
    progress("DSH 安装完成并已通过校验");
    return true;
}

std::optional<std::string> Service::latest_version() { return platform::latest_dsh_version(); }
std::string Service::node_version() { return platform::node_version(); }
std::string Service::npm_version() { return platform::npm_version(); }
const std::filesystem::path& Service::log_path() const noexcept { return log_.path(); }
std::filesystem::path Service::service_log_path() const { return state_directory_ / "logs" / "dsh-web.log"; }

std::optional<std::uint32_t> Service::read_pid() const {
    std::ifstream stream(pid_file_);
    std::uint64_t pid{};
    if (!(stream >> pid) || pid == 0 || pid > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(pid);
}

bool Service::write_pid(std::uint32_t pid, std::string& error) const {
    std::ofstream stream(pid_file_, std::ios::trunc);
    if (!(stream << pid)) {
        error = "无法写入 DSH 进程记录。";
        return false;
    }
    return true;
}

void Service::clear_pid() const {
    std::error_code error;
    std::filesystem::remove(pid_file_, error);
}

}  // namespace dsh

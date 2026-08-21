#include "core/service.h"

#include "platform/platform.h"
#include "core/semver.h"
#include "core/version.h"

#include <fstream>

namespace dsh {

Service::Service()
    : state_directory_(platform::state_directory()),
      pid_file_(state_directory_ / "dsh-web.pid"),
      log_(state_directory_) {
    std::filesystem::create_directories(state_directory_);
    log_.info(std::string("Launcher service initialized; version=") + launcher_version);
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

UpdateResult Service::update_dsh(const Progress& progress, const ConfirmDshUpdate& confirm) {
    UpdateResult result;
    const auto status = detect();
    result.current = status.version;
    if (!status.installed || status.version.empty()) {
        result.message = "未找到可更新的 DSH";
        return result;
    }
    const auto latest = platform::latest_dsh_version();
    if (!latest) {
        result.message = "暂时无法连接 DSH 更新源";
        log_.error("DSH update check failed");
        return result;
    }
    result.checked = true;
    result.latest = *latest;
    if (!version::is_newer(*latest, status.version)) {
        result.message = "DSH 已是最新版本 " + status.version;
        return result;
    }

    result.available = true;
    if (!confirm(status.version, *latest, status.executable)) {
        result.message = "已暂不更新 DSH，继续使用当前版本 " + status.version;
        return result;
    }
    if (status.running) {
        progress("正在停止旧版本 DSH，更新完成后会自动重启");
        std::string stop_error;
        if (!stop(stop_error)) {
            result.message = "当前 DSH 服务无法安全停止，已取消更新：" + stop_error;
            return result;
        }
    }
    progress("发现 DSH " + *latest + "，正在后台更新");
    std::string error;
    if (!platform::update_dsh_at(status.executable, error)) {
        result.message = "DSH 更新失败，稍后将自动重试";
        log_.error("DSH update failed: " + error);
        return result;
    }
    const auto updated_version = platform::dsh_version(status.executable);
    if (!version::is_newer(updated_version, status.version) && updated_version != *latest) {
        result.message = "DSH 更新校验未通过";
        log_.error("DSH update verification failed; expected=" + *latest + "; actual=" + updated_version);
        return result;
    }
    result.completed = true;
    result.message = "DSH 已在原安装目录更新到 " + updated_version;
    log_.info("DSH updated in place; from=" + status.version + "; to=" + updated_version + "; executable=" + status.executable);
    return result;
}

UpdateResult Service::update_launcher(const Progress& progress, const ConfirmLauncherUpdate& confirm) {
    UpdateResult result;
    result.current = launcher_version;
    std::string error;
    const auto update = platform::launcher_update_manifest(error);
    if (!update) {
        result.message = "暂时无法连接启动器更新源";
        log_.error("Launcher update check failed: " + error);
        return result;
    }
    result.checked = true;
    result.latest = update->version;
    if (!version::is_newer(update->version, launcher_version)) {
        result.message = std::string("启动器已是最新版本 ") + launcher_version;
        return result;
    }
    result.available = true;
    if (!confirm(launcher_version, update->version)) {
        result.message = std::string("已暂不更新启动器，继续使用 ") + launcher_version;
        return result;
    }
    progress("发现启动器 " + update->version + "，正在后台下载");
    if (!platform::stage_launcher_update(*update, error)) {
        result.message = "启动器更新下载失败，稍后将自动重试";
        log_.error("Launcher update staging failed: " + error);
        return result;
    }
    result.completed = true;
    result.message = "启动器 " + update->version + " 已就绪，关闭窗口后自动替换";
    log_.info("Launcher update staged; from=" + std::string(launcher_version) + "; to=" + update->version);
    return result;
}

std::optional<std::string> Service::latest_version() { return platform::latest_dsh_version(); }
const std::filesystem::path& Service::log_path() const noexcept { return log_.path(); }

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

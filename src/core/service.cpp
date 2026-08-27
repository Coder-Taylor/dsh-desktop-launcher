#include "core/service.h"

#include "core/semver.h"
#include "platform/platform.h"

#include <chrono>
#include <fstream>
#include <thread>

namespace dsh {

Service::Service()
    : state_directory_(platform::state_directory()),
      pid_file_(state_directory_ / "dsh-web.pid"),
      settings_file_(state_directory_ / "settings.ini"),
      log_(state_directory_) {
    std::filesystem::create_directories(state_directory_);
    std::ifstream settings(settings_file_);
    std::string line;
    while (std::getline(settings, line)) {
        if (line == "source=official") install_source_ = InstallSource::official;
        if (line == "source=mirror") install_source_ = InstallSource::mirror;
    }
    log_.info("Launcher service initialized; version=0.1.1-beta.2; state=" + state_directory_.string());
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

bool Service::is_running() const { return platform::is_web_running(); }

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
    std::string terminate_error;
    const bool terminate_reported_success = platform::stop_process_tree(*pid, terminate_error);
    // taskkill can report its own timeout while the target process tree has
    // already been killed. For an update, the authoritative condition is that
    // DSH no longer owns the local web port, not taskkill's exit code.
    for (int attempt = 0; attempt < 20 && platform::is_web_running(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (platform::is_web_running()) {
        error = terminate_reported_success
                    ? "已请求停止 DSH，但 3080 端口仍在响应。"
                    : "无法停止 DSH，且 3080 端口仍在响应：" + terminate_error;
        log_.error("Failed to stop DSH: " + error);
        return false;
    }
    clear_pid();
    if (!terminate_reported_success) {
        log_.info("DSH stopped after taskkill reported a late timeout: pid=" + std::to_string(*pid));
    }
    log_.info("DSH stopped; pid=" + std::to_string(*pid));
    return true;
}

bool Service::open_web(std::string& error) { return platform::open_web(error); }

bool Service::ensure_installed(const Progress& progress, std::string& error) {
    return install_at(default_dsh_directory(), install_source_, progress, error);
}

bool Service::install_at(const std::filesystem::path& directory, InstallSource source,
                         const Progress& progress, std::string& error,
                         const std::atomic_bool* cancel,
                         const NodeInstallOptions* node_options) {
    const auto report = [this, &progress](const std::string& message) {
        log_.info("install phase: " + message);
        if (progress) progress(message);
    };
    if (cancel && cancel->load()) {
        error = "用户已取消安装。";
        return false;
    }
    if (!platform::has_node() || !platform::has_npm()) {
        const bool use_system_node = node_options && node_options->install_system_node;
        report(use_system_node ? "未检测到完整的 Node.js 环境，准备安装系统 Node.js LTS"
                               : "未检测到完整的 Node.js 环境，正在安装兼容运行环境");
        log_.info(use_system_node ? "Installing system Node.js runtime" : "Installing managed Node.js runtime");
        const bool node_installed = use_system_node
            ? platform::install_system_node(node_options->source == InstallSource::official,
                                            node_options->directory, error, cancel)
            : platform::install_managed_node(source == InstallSource::official, error, cancel);
        if (!node_installed) {
            log_.error(std::string(use_system_node ? "System" : "Managed") +
                       " Node.js installation failed: " + error);
            return false;
        }
        report("Node.js 和 npm 已安装");
    } else {
        report("Node.js " + platform::node_version() + "，npm " + platform::npm_version());
    }

    report(source == InstallSource::official ? "正在从 npm 官方源安装 DSH"
                                             : "正在探测国内 npm 镜像并安装 DSH");
    log_.info("Installing DSH runtime; directory=" + directory.string() +
              (source == InstallSource::official ? "; source=official" : "; source=mirror"));
    if (!platform::install_dsh_at(directory, source == InstallSource::official, error, {}, cancel, report)) {
        log_.error("Managed DSH installation failed: " + error);
        return false;
    }
    if (!platform::find_dsh()) {
        error = "安装命令已完成，但未找到 DSH 可执行文件。";
        return false;
    }
    report("DSH 安装完成并已通过校验");
    return true;
}

bool Service::uninstall(bool remove_dsh, bool remove_node, bool preserve_conversation_memory,
                        const Progress& progress, std::string& error) {
    const auto report = [this, &progress](const std::string& message) {
        log_.info("uninstall phase: " + message);
        if (progress) progress(message);
    };
    if (remove_dsh && platform::is_web_running()) {
        report("正在停止 DSH 后台服务");
        if (!stop(error)) return false;
    }
    if (remove_dsh) {
        report("正在卸载 DSH 程序文件");
        if (!platform::uninstall_dsh(error)) {
            log_.error("DSH uninstall failed: " + error);
            return false;
        }
        if (!preserve_conversation_memory) {
            report("正在清除 DSH 对话记忆和本地存储");
            if (!platform::clear_conversation_memory(error)) {
                log_.error("DSH conversation-memory cleanup failed: " + error);
                return false;
            }
        }
    }
    if (remove_node) {
        report("正在卸载由启动器安装的 Node.js");
        if (!platform::uninstall_launcher_owned_node(error)) {
            log_.error("Launcher-owned Node.js uninstall failed: " + error);
            return false;
        }
    }
    log_.info(remove_dsh && remove_node ? "DSH and managed Node.js uninstalled"
             : remove_dsh ? "DSH uninstalled; managed Node.js preserved"
                          : "Managed Node.js uninstalled; DSH preserved");
    report(remove_dsh && !preserve_conversation_memory
               ? "卸载完成，对话记忆已清除；设置、凭据和日志已保留"
               : "卸载完成，用户配置、会话和日志已保留");
    return true;
}

bool Service::update(const Progress& progress, std::string& error,
                     const std::atomic_bool* cancel) {
    const auto report = [this, &progress](const std::string& message) {
        log_.info("update phase: " + message);
        if (progress) progress(message);
    };
    log_.info("update phase: detect current DSH");
    if (cancel && cancel->load()) {
        error = "用户已取消更新。";
        return false;
    }
    const auto current = detect();
    if (!current.installed || current.executable.empty()) {
        error = "未找到可更新的 DSH 安装。";
        return false;
    }
    const auto latest = latest_version(cancel);
    if (!latest) {
        error = cancel && cancel->load()
                    ? "用户已取消更新源检查。"
                    : "无法连接 DSH 更新源，国内镜像和官方源均未返回版本信息；请检查 HTTPS 网络、系统代理或 TUN 设置。";
        return false;
    }
    if (!current.version.empty() && !version::is_newer(*latest, current.version)) {
        report("DSH 已是最新版本 " + current.version);
        return true;
    }
    const auto* utf8_begin = reinterpret_cast<const char8_t*>(current.executable.data());
    const std::u8string executable_utf8(utf8_begin, utf8_begin + current.executable.size());
    auto prefix = std::filesystem::path(executable_utf8);
    prefix = prefix.parent_path();
    if (prefix.filename() == ".bin" && prefix.parent_path().filename() == "node_modules") {
        prefix = prefix.parent_path().parent_path();
    }
    if (current.running) {
        report("正在停止当前 DSH 服务");
        if (!stop(error)) return false;
        for (int attempt = 0; attempt < 25 && platform::is_web_running(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (platform::is_web_running()) {
            error = "DSH 服务尚未完全退出，已取消更新以避免覆盖正在使用的文件。";
            log_.error("DSH update aborted because the service is still listening on port 3080");
            return false;
        }
        report("DSH 服务已停止，开始更新文件");
    }
    report("正在原安装目录更新 DSH " + *latest + "（依赖较多，可能需要数分钟）");
    if (!platform::install_dsh_at(prefix, install_source_ == InstallSource::official, error, *latest, cancel, report)) {
        log_.error("DSH update failed: " + error);
        return false;
    }
    const auto verified = detect();
    if (!verified.installed || verified.version.empty() || version::is_newer(*latest, verified.version)) {
        error = "版本校验未通过：期望 " + *latest + "，实际 " +
                (verified.version.empty() ? std::string("未知") : verified.version) +
                "；检测路径=" + verified.executable;
        log_.error("DSH update verification failed; expected=" + *latest + ", actual=" + verified.version);
        return false;
    }
    log_.info("DSH updated in place; from=" + current.version + "; to=" + verified.version);
    report("DSH 更新完成并通过版本校验");
    return true;
}

bool Service::update_launcher(const platform::LauncherUpdate& update, const Progress& progress,
                              std::string& error, const std::atomic_bool* cancel) {
    if (cancel && cancel->load()) {
        error = "用户已取消启动器更新。";
        return false;
    }
    if (progress) progress("正在下载启动器更新包 " + update.version);
    if (!platform::update_launcher(update, error, cancel)) {
        log_.error("Launcher update failed: " + error);
        return false;
    }
    log_.info("Launcher update package verified and replacement scheduled; version=" + update.version);
    if (progress) progress("启动器更新包已校验，正在重启启动器");
    return true;
}

EnvironmentStatus Service::environment() {
    EnvironmentStatus result;
    result.has_node = platform::has_node();
    result.has_npm = platform::has_npm();
    if (result.has_node) result.node_version = platform::node_version();
    if (result.has_npm) result.npm_version = platform::npm_version();
    return result;
}

bool Service::managed_node_installed() const { return platform::has_managed_node(); }
bool Service::launcher_owned_node_installed() const { return platform::has_launcher_owned_node(); }

std::filesystem::path Service::default_dsh_directory() const { return platform::default_dsh_directory(); }
InstallSource Service::install_source() const noexcept { return install_source_; }

bool Service::set_install_source(InstallSource source, std::string& error) {
    const auto temporary = std::filesystem::path(settings_file_.wstring() + L".tmp");
    {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << (source == InstallSource::official ? "source=official\n" : "source=mirror\n");
        if (!stream.good()) {
            error = "无法保存更新源设置。";
            return false;
        }
    }
    std::error_code filesystem_error;
    std::filesystem::remove(settings_file_, filesystem_error);
    filesystem_error.clear();
    std::filesystem::rename(temporary, settings_file_, filesystem_error);
    if (filesystem_error) {
        error = "无法保存更新源设置。";
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    install_source_ = source;
    log_.info(source == InstallSource::official ? "Install source changed to official"
                                                : "Install source changed to mirror");
    return true;
}

std::optional<std::string> Service::latest_version(const std::atomic_bool* cancel) {
    std::string diagnostic;
    const auto result = platform::latest_dsh_version(
        install_source_ == InstallSource::official, cancel, &diagnostic);
    if (result) {
        log_.info("update phase: source check succeeded; latest=" + *result);
    } else {
        log_.error("update phase: source check failed; " +
                   (diagnostic.empty() ? "no registry returned usable metadata" : diagnostic));
    }
    return result;
}
std::optional<platform::LauncherUpdate> Service::latest_launcher_update(const std::atomic_bool* cancel) {
    std::string diagnostic;
    const auto result = platform::latest_launcher_update(
        install_source_ == InstallSource::official, cancel, &diagnostic);
    if (result) {
        log_.info("launcher update check succeeded; latest=" + result->version);
    } else {
        log_.info("launcher update check unavailable; " +
                  (diagnostic.empty() ? "manifest not configured or unreachable" : diagnostic));
    }
    return result;
}
std::string Service::node_version() { return platform::node_version(); }
std::string Service::npm_version() { return platform::npm_version(); }
const std::filesystem::path& Service::log_path() const noexcept { return log_.path(); }
const std::filesystem::path& Service::fallback_log_path() const noexcept { return log_.fallback_path(); }
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

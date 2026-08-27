#ifdef _WIN32

#include "platform/platform.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

namespace dsh::platform {
namespace {

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

struct CommandResult {
    std::string output;
    DWORD exit_code{1};
    bool timed_out{};
    bool cancelled{};
    std::uint64_t elapsed_ms{};
    std::size_t peak_job_memory{};
    std::size_t job_memory_limit{};
};

CommandResult capture(const std::string& command, DWORD timeout_ms = 30000,
                      std::size_t job_memory_limit = 512ULL * 1024 * 1024,
                      const std::atomic_bool* cancel = nullptr,
                      const std::function<void(const std::string&)>& progress = {}) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return {};
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    auto command_line = std::wstring(L"cmd.exe /d /s /c \"") + utf8_to_wide(command) + L"\"";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return {};
    }
    const auto job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        TerminateProcess(process.hProcess, ERROR_NOT_ENOUGH_MEMORY);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(write_pipe);
        CloseHandle(read_pipe);
        return {"无法创建受限进程组，已拒绝无上限运行外部命令。", ERROR_NOT_ENOUGH_MEMORY, false};
    }
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_memory_limit > 0) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            limits.JobMemoryLimit = static_cast<SIZE_T>(job_memory_limit);
        }
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            TerminateProcess(process.hProcess, ERROR_ACCESS_DENIED);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(write_pipe);
            CloseHandle(read_pipe);
            return {"无法配置受限进程组，已拒绝无上限运行外部命令。", ERROR_ACCESS_DENIED, false};
        }
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    CloseHandle(write_pipe);

    CommandResult result;
    std::array<char, 512> buffer{};
    // Keep a small line buffer for the activity log.  npm usually stays quiet
    // while resolving its dependency tree, but when it does report a network
    // or package error the user should see it immediately instead of only
    // after the five-minute command timeout.
    std::string live_output;
    const auto report_live_output = [&] {
        if (!progress) return;
        for (;;) {
            const auto newline = live_output.find_first_of("\r\n");
            if (newline == std::string::npos) break;
            const auto line = trim(live_output.substr(0, newline));
            live_output.erase(0, live_output.find_first_not_of("\r\n", newline));
            if (!line.empty()) progress("npm：" + line);
        }
        // Do not retain an unbounded line if a program writes a progress bar
        // without line endings.
        if (live_output.size() > 2048) {
            progress("npm：" + trim(live_output.substr(0, 2048)));
            live_output.clear();
        }
    };
    const auto drain_output = [&] {
        for (;;) {
            DWORD available{};
            if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            DWORD bytes_read{};
            const auto wanted = static_cast<DWORD>((std::min)(buffer.size(), static_cast<std::size_t>(available)));
            if (!ReadFile(read_pipe, buffer.data(), wanted, &bytes_read, nullptr) || bytes_read == 0) break;
            if (result.output.size() < 16384) {
                result.output.append(buffer.data(), (std::min)(static_cast<std::size_t>(bytes_read),
                                                               16384 - result.output.size()));
            }
            if (progress) {
                live_output.append(buffer.data(), bytes_read);
                report_live_output();
            }
        }
    };
    const auto started = GetTickCount64();
    std::uint64_t next_heartbeat_ms = 15000;
    for (;;) {
        drain_output();
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) break;
        const auto elapsed_ms = GetTickCount64() - started;
        if (progress && elapsed_ms >= next_heartbeat_ms) {
            progress("npm 正在处理依赖或下载中；已用 " +
                     std::to_string(elapsed_ms / 1000) + " 秒");
            next_heartbeat_ms += 15000;
        }
        if (cancel && cancel->load()) {
            result.cancelled = true;
            result.exit_code = ERROR_CANCELLED;
            if (job) TerminateJobObject(job, ERROR_CANCELLED);
            else TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, 5000);
            result.output += "\n用户已取消操作，相关进程已终止。";
            break;
        }
        if (elapsed_ms >= timeout_ms) {
            result.timed_out = true;
            result.exit_code = ERROR_TIMEOUT;
            if (job) TerminateJobObject(job, ERROR_TIMEOUT);
            else TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 5000);
            result.output += "\n命令执行超时，已终止相关进程。";
            break;
        }
    }
    if (!result.timed_out) GetExitCodeProcess(process.hProcess, &result.exit_code);
    result.elapsed_ms = GetTickCount64() - started;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION final_limits{};
    if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &final_limits,
                                  sizeof(final_limits), nullptr)) {
        result.peak_job_memory = static_cast<std::size_t>(final_limits.PeakJobMemoryUsed);
        result.job_memory_limit = static_cast<std::size_t>(final_limits.JobMemoryLimit);
    }
    drain_output();
    if (progress && !trim(live_output).empty()) progress("npm：" + trim(live_output));
    if (job) CloseHandle(job);
    drain_output();
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    result.output = trim(result.output);
    return result;
}

std::string first_line(const std::string& value) {
    const auto end = value.find_first_of("\r\n");
    return trim(value.substr(0, end));
}

std::optional<std::string> extract_version(const std::string& output) {
    // npm registry endpoints return JSON such as
    // {"version":"0.1.1-rc.2"}. The legacy matcher only accepted versions
    // surrounded by whitespace, so valid registry metadata was treated as an
    // unavailable update check.
    static const std::regex json_version_pattern(
        R"json("version"\s*:\s*"v?(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)")json");
    std::smatch match;
    if (std::regex_search(output, match, json_version_pattern)) return match[1].str();
    static const std::regex pattern(R"((?:^|\s)v?(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)(?:\s|$))");
    if (std::regex_search(output, match, pattern)) return match[1].str();
    return std::nullopt;
}

bool run_elevated_and_wait(const std::wstring& file, const std::wstring& parameters,
                           std::string& error, const std::atomic_bool* cancel = nullptr) {
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = nullptr;
    execute.lpVerb = L"runas";
    execute.lpFile = file.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute) || !execute.hProcess) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
            ? "已取消管理员授权，Node.js 尚未安装。"
            : "无法启动需要管理员权限的 Node.js 安装器（Windows 错误码 " + std::to_string(code) + "）。";
        return false;
    }
    // An MSI/winget installation owns the elevated child process.  We do not
    // kill it on a UI cancel request: doing so can leave Windows Installer in
    // an incomplete state.  Cancellation is honoured immediately afterwards.
    while (WaitForSingleObject(execute.hProcess, 200) == WAIT_TIMEOUT) {}
    DWORD exit_code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(execute.hProcess, &exit_code);
    CloseHandle(execute.hProcess);
    if (cancel && cancel->load()) {
        error = "用户在 Node.js 安装完成后取消了后续 DSH 安装。";
        return false;
    }
    if (exit_code != 0) {
        error = "Node.js 安装器返回失败代码 " + std::to_string(exit_code) + "。";
        return false;
    }
    return true;
}

std::wstring quote(const std::filesystem::path& value) {
    return L"\"" + value.wstring() + L"\"";
}

std::filesystem::path runtime_directory() {
    return state_directory() / "runtime";
}

std::string path_utf8(const std::filesystem::path& value);

std::filesystem::path managed_node_directory() {
    return runtime_directory() / "node";
}

std::filesystem::path remembered_node_file() {
    return state_directory() / "node-location.txt";
}

std::filesystem::path launcher_owned_node_file() {
    return state_directory() / "node-installed-by-launcher.txt";
}

std::filesystem::path configured_node_directory() {
    std::ifstream stream(remembered_node_file());
    std::string value;
    std::getline(stream, value);
    const auto path = std::filesystem::path(utf8_to_wide(trim(value)));
    return std::filesystem::exists(path / "node.exe") ? path : std::filesystem::path{};
}

std::filesystem::path preferred_node_directory() {
    if (std::filesystem::exists(managed_node_directory() / "node.exe")) return managed_node_directory();
    return configured_node_directory();
}

void remember_node_directory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(state_directory(), error);
    std::ofstream stream(remembered_node_file(), std::ios::trunc);
    stream << path_utf8(directory);
}

void remember_launcher_owned_node(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(state_directory(), error);
    std::ofstream stream(launcher_owned_node_file(), std::ios::trunc);
    stream << "system\n" << path_utf8(directory);
}

std::filesystem::path launcher_owned_node_directory() {
    std::ifstream stream(launcher_owned_node_file());
    std::string kind;
    std::string location;
    std::getline(stream, kind);
    std::getline(stream, location);
    if (kind != "system" || location.empty()) return {};
    return std::filesystem::path(utf8_to_wide(trim(location)));
}

void clear_launcher_owned_node() {
    std::error_code error;
    std::filesystem::remove(launcher_owned_node_file(), error);
}

std::filesystem::path managed_dsh_command() {
    return runtime_directory() / "dsh" / "dsh.cmd";
}

// Keep the domestic path aligned with the original BAT: use npmmirror first,
// then fall back to npmjs only when that explicit source is unavailable. A
// metadata check must never silently fan out across unrelated mirrors.
constexpr std::array<const char*, 1> domestic_npm_registries{
    "https://registry.npmmirror.com"};
constexpr const char* official_npm_registry = "https://registry.npmjs.org";
constexpr const char* launcher_manifest_gitee =
    "https://gitee.com/taylorchengitee/dsh-desktop-launcher/raw/main/packaging/manifests/update-manifest.json";
constexpr const char* launcher_manifest_github =
    "https://raw.githubusercontent.com/Coder-Taylor/dsh-desktop-launcher/main/packaging/manifests/update-manifest.json";

bool has_update_memory_budget(std::string& error) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return true;
    constexpr ULONGLONG minimum_available = 3ULL * 1024 * 1024 * 1024;
    if (memory.ullAvailPhys < minimum_available) {
        error = "系统可用内存不足（当前约 " +
                std::to_string(memory.ullAvailPhys / (1024 * 1024)) +
                " MiB，至少需要 3072 MiB），已停止安装/更新以避免卡死。";
        return false;
    }
    return true;
}

struct NpmMemoryPolicy {
    std::size_t job_limit{};
    unsigned int v8_heap_mib{};
};

NpmMemoryPolicy npm_memory_policy() {
    constexpr std::size_t gib = 1024ULL * 1024ULL * 1024ULL;
    constexpr std::size_t minimum_job_limit = 2ULL * gib;
    constexpr std::size_t maximum_job_limit = 4ULL * gib;
    constexpr std::size_t system_reserve = 1ULL * gib;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory) || memory.ullAvailPhys <= system_reserve) {
        return {minimum_job_limit, 1536};
    }
    const auto available_after_reserve = static_cast<std::size_t>(memory.ullAvailPhys - system_reserve);
    const auto job_limit = (std::min)(maximum_job_limit,
                                      (std::max)(minimum_job_limit, available_after_reserve));
    const auto v8_heap_mib = job_limit >= 3584ULL * 1024ULL * 1024ULL ? 3072U
                           : job_limit >= 2560ULL * 1024ULL * 1024ULL ? 2048U
                                                                          : 1536U;
    return {job_limit, v8_heap_mib};
}

std::filesystem::path remembered_dsh_file() {
    return state_directory() / "dsh-location.txt";
}

std::filesystem::path dsh_home_directory() {
    const DWORD required = GetEnvironmentVariableW(L"DSH_HOME", nullptr, 0);
    if (required > 1) {
        std::wstring configured(required, L'\0');
        if (GetEnvironmentVariableW(L"DSH_HOME", configured.data(), required) > 0) {
            configured.resize(configured.find(L'\0'));
            if (!configured.empty()) return configured;
        }
    }
    wchar_t* profile = nullptr;
    std::size_t length{};
    if (_wdupenv_s(&profile, &length, L"USERPROFILE") == 0 && profile) {
        const std::filesystem::path result = std::filesystem::path(profile) / L".dsh";
        free(profile);
        return result;
    }
    if (profile) free(profile);
    return {};
}

void remember_dsh(const std::filesystem::path& executable) {
    std::error_code error;
    std::filesystem::create_directories(state_directory(), error);
    std::ofstream stream(remembered_dsh_file(), std::ios::trunc);
    stream << path_utf8(executable);
}

void clear_remembered_dsh() {
    std::error_code error;
    std::filesystem::remove(remembered_dsh_file(), error);
}

std::string path_utf8(const std::filesystem::path& value) {
    const auto wide = value.wstring();
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::optional<std::string> local_http_proxy() {
    // Do not guess a Clash/mihomo port. An unrelated local service can be
    // listening on a popular port, after which npm waits behind a proxy it was
    // never configured to use. WinHTTP already reads Windows system proxy
    // settings; npm fallback only uses an explicit environment proxy.
    for (const char* name : {"HTTPS_PROXY", "HTTP_PROXY"}) {
        if (const char* value = std::getenv(name); value && *value) {
            return trim(value);
        }
    }
    return std::nullopt;
}

std::string winhttp_proxy_address(std::string value) {
    value = trim(std::move(value));
    if (value.rfind("http://", 0) == 0) value.erase(0, 7);
    else if (value.rfind("https://", 0) == 0) value.erase(0, 8);
    const auto path = value.find('/');
    if (path != std::string::npos) value.erase(path);
    return value;
}

std::string describe_winhttp_error(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "目标 443 端口不可达（当前连接是直连或代理端口未监听）";
    case ERROR_WINHTTP_TIMEOUT:
        return "连接超时（当前连接是直连或代理没有返回）";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "DNS 无法解析主机名";
    case ERROR_WINHTTP_CLIENT_CERT_NO_PRIVATE_KEY:
        return "代理要求客户端证书，但当前账户没有可用私钥；通常是 HTTPS 代理/TLS 中间人配置问题";
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED_PROXY:
        return "HTTPS 代理要求客户端证书，启动器无法完成代理认证";
    case ERROR_WINHTTP_SECURE_FAILURE_PROXY:
        return "HTTPS 代理 TLS 握手失败；请检查 Clash/Verge 的 HTTPS 解密证书或关闭该域名的 TLS 中间人";
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "HTTPS TLS 握手失败；未跳过证书校验";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "网络连接被重置或中途断开";
    default:
        return {};
    }
}

bool download(const std::string& url, std::vector<unsigned char>& body, std::string& error,
              std::size_t limit = 128 * 1024 * 1024,
              const std::atomic_bool* cancel = nullptr,
              bool use_default_proxy = false,
              const std::string& explicit_proxy = {}) {
    if (cancel && cancel->load()) {
        error = "用户已取消下载。";
        return false;
    }
    const auto wide_url = utf8_to_wide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
        error = "下载地址无效。";
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const auto open_session = [](DWORD access_type, const std::wstring& proxy) {
        return WinHttpOpen(L"DshLauncher/0.1", access_type,
                           proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),
                           WINHTTP_NO_PROXY_BYPASS, 0);
    };
    const auto proxy_wide = utf8_to_wide(winhttp_proxy_address(explicit_proxy));
    const auto access_type = !explicit_proxy.empty()
                                 ? WINHTTP_ACCESS_TYPE_NAMED_PROXY
                                 : use_default_proxy ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                                                     : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    auto session = open_session(access_type, proxy_wide);
    if (!session) { error = "无法初始化 Windows 网络组件。"; return false; }
    // Metadata probes are tiny and should fail quickly; package archives get
    // the longer receive window.
    const DWORD receive_timeout = limit <= 4 * 1024 * 1024 ? 5000 : 15000;
    WinHttpSetTimeouts(session, 3000, 3000, 5000, receive_timeout);
    const auto retry_with_local_proxy = [&](std::string& retry_error) {
        if (use_default_proxy || !explicit_proxy.empty()) return false;
        std::vector<std::string> candidates;
        for (const char* name : {"HTTPS_PROXY", "HTTP_PROXY"}) {
            if (const char* value = std::getenv(name); value && *value) candidates.emplace_back(value);
        }
        for (const auto& proxy : candidates) {
            if (cancel && cancel->load()) {
                retry_error = "用户已取消下载。";
                return false;
            }
            std::vector<unsigned char> retry_body;
            std::string candidate_error;
            if (download(url, retry_body, candidate_error, limit, cancel, false, proxy)) {
                body = std::move(retry_body);
                return true;
            }
            // Keep every candidate's failure.  The last candidate is often a
            // closed port (12029), which used to hide the useful TLS error
            // returned by the proxy that actually accepted the connection.
            if (!retry_error.empty()) retry_error += "; ";
            retry_error += proxy + " => " + candidate_error;
        }
        return false;
    };
    auto connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection) {
        const auto code = GetLastError();
        WinHttpCloseHandle(session);
        if (!use_default_proxy &&
            (code == ERROR_WINHTTP_CANNOT_CONNECT || code == ERROR_WINHTTP_TIMEOUT)) {
            if (cancel && cancel->load()) {
                error = "用户已取消下载。";
                return false;
            }
            std::vector<unsigned char> retry_body;
            std::string retry_error;
            if (download(url, retry_body, retry_error, limit, cancel, true)) {
                body = std::move(retry_body);
                return true;
            }
            if (cancel && cancel->load()) {
                error = "用户已取消下载。";
                return false;
            }
            if (retry_with_local_proxy(retry_error)) return true;
            error = retry_error;
            return false;
        }
        error = "无法连接 " + url + "（Windows 错误码 " + std::to_string(code);
        if (const auto explanation = describe_winhttp_error(code); !explanation.empty()) {
            error += "：" + explanation;
        }
        error += "；请检查系统代理、HTTPS 网络或 TUN 设置）。";
        return false;
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const auto request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    DWORD network_error = ERROR_SUCCESS;
    bool ok = request != nullptr;
    if (!ok) network_error = GetLastError();
    if (ok && !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        network_error = GetLastError();
        ok = false;
    }
    if (ok && !WinHttpReceiveResponse(request, nullptr)) {
        network_error = GetLastError();
        ok = false;
    }
    if (cancel && cancel->load()) {
        error = "用户已取消下载。";
        ok = false;
    }
    DWORD status{};
    DWORD status_size = sizeof(status);
    if (ok) {
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX) && status >= 200 && status < 300;
        if (!ok && status == 0) network_error = GetLastError();
    }
    body.clear();
    while (ok) {
        if (cancel && cancel->load()) {
            error = "用户已取消下载。";
            ok = false;
            break;
        }
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request, &available)) {
            network_error = GetLastError();
            ok = false;
            break;
        }
        if (available == 0) break;
        if (body.size() + available > limit) {
            error = "下载内容超过安全大小限制。";
            ok = false;
            break;
        }
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD bytes_read{};
        if (!WinHttpReadData(request, body.data() + offset, available, &bytes_read)) {
            network_error = GetLastError();
            ok = false;
            break;
        }
        body.resize(offset + bytes_read);
    }
    if (request) WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok || body.empty()) {
        if (cancel && cancel->load()) {
            error = "用户已取消下载。";
            return false;
        }
        if (!use_default_proxy && status == 0 &&
            (network_error == ERROR_WINHTTP_CANNOT_CONNECT ||
             network_error == ERROR_WINHTTP_TIMEOUT)) {
            std::vector<unsigned char> retry_body;
            std::string retry_error;
            if (download(url, retry_body, retry_error, limit, cancel, true)) {
                body = std::move(retry_body);
                return true;
            }
            if (cancel && cancel->load()) {
                error = "用户已取消下载。";
                return false;
            }
            if (retry_with_local_proxy(retry_error)) return true;
            error = retry_error;
            return false;
        }
        if (error.empty()) {
            if (status != 0) {
                error = "服务器返回 HTTP " + std::to_string(status) + "：" + url;
            } else {
                error = "下载服务器没有返回有效内容（Windows 错误码 " +
                        std::to_string(network_error);
                if (const auto explanation = describe_winhttp_error(network_error); !explanation.empty()) {
                    error += "：" + explanation;
                }
                error += "）。";
            }
        }
        return false;
    }
    return true;
}

bool write_binary(const std::filesystem::path& path, const std::vector<unsigned char>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    return stream.write(reinterpret_cast<const char*>(data.data()),
                        static_cast<std::streamsize>(data.size())).good();
}

std::string executable_command(const std::filesystem::path& managed, const char* fallback) {
    if (std::filesystem::exists(managed)) return "\"" + path_utf8(managed) + "\"";
    return fallback;
}

std::optional<std::string> npm_query_version(const char* registry,
                                             const std::atomic_bool* cancel,
                                             std::string* diagnostic) {
    if (!has_node() || !has_npm()) return std::nullopt;
    auto cache = state_directory() / "npm-cache";
    std::error_code filesystem_error;
    std::filesystem::create_directories(cache, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(cache)) {
        filesystem_error.clear();
        cache = std::filesystem::temp_directory_path() / "DshLauncher" / "npm-cache";
        std::filesystem::create_directories(cache, filesystem_error);
    }
    if (filesystem_error || !std::filesystem::is_directory(cache)) return std::nullopt;
    const auto proxy = local_http_proxy();
    std::string command = "set \"npm_config_cache=" + path_utf8(cache) + "\" && "
                          "set \"npm_config_update_notifier=false\" && ";
    if (proxy) {
        command += "set \"HTTP_PROXY=" + *proxy + "\" && set \"HTTPS_PROXY=" + *proxy + "\" && ";
    }
    command += executable_command(preferred_node_directory() / "npm.cmd", "npm.cmd") +
               " view @deepseek-ai/dsh version --registry=" + registry +
               " --fetch-timeout=10000 --fetch-retries=0 --json --loglevel=error 2>&1";
    const auto result = capture(command, 20000, 256ULL * 1024 * 1024, cancel);
    if (result.cancelled || (cancel && cancel->load())) return std::nullopt;
    auto output = trim(result.output);
    if (output.size() >= 2 && output.front() == '\"' && output.back() == '\"') {
        output = output.substr(1, output.size() - 2);
    }
    if (const auto version = extract_version(output)) return version;
    if (diagnostic) {
        auto summary = output;
        if (summary.size() > 240) summary.resize(240);
        if (!diagnostic->empty()) *diagnostic += "; ";
        *diagnostic += std::string(registry) + " => npm fallback failed" +
                       (summary.empty() ? std::string() : ": " + summary);
    }
    return std::nullopt;
}

bool node_download(const std::string& url, std::vector<unsigned char>& body,
                   std::string& error, std::size_t limit,
                   const std::atomic_bool* cancel) {
    if (!has_node()) return false;
    const auto npm_path = first_line(capture("where.exe npm.cmd 2>nul").output);
    const auto node_root = npm_path.empty()
                               ? preferred_node_directory()
                               : std::filesystem::path(utf8_to_wide(npm_path)).parent_path();
    const auto node_modules = node_root / "node_modules" / "npm" / "node_modules";
    const auto node = executable_command(preferred_node_directory() / "node.exe", "node.exe");
    const auto temp = std::filesystem::temp_directory_path() / "DshLauncher";
    std::error_code filesystem_error;
    std::filesystem::create_directories(temp, filesystem_error);
    if (filesystem_error) return false;
    const auto script = temp / "proxy-fetch.js";
    if (!std::filesystem::exists(script)) {
        static constexpr char script_source[] = R"JS(const fs = require('fs');
const http = require('http');
const https = require('https');
const { URL } = require('url');
let HttpsProxyAgent;
try { HttpsProxyAgent = require('https-proxy-agent').HttpsProxyAgent; } catch (_) {}
const target = process.argv[2];
const output = process.argv[3];
const maxRedirects = 5;
function fail(message) { process.stderr.write(String(message)); process.exitCode = 1; }
function fetchUrl(address, depth) {
  if (depth > maxRedirects) return fail('too many redirects');
  const parsed = new URL(address);
  const secure = parsed.protocol === 'https:';
  const proxy = process.env.HTTPS_PROXY || process.env.HTTP_PROXY;
  const options = {
    hostname: parsed.hostname,
    port: parsed.port || (secure ? 443 : 80),
    path: parsed.pathname + parsed.search,
    method: 'GET',
    headers: { 'User-Agent': 'DshLauncher/0.1' }
  };
  if (secure && proxy && HttpsProxyAgent) options.agent = new HttpsProxyAgent(proxy);
  const request = (secure ? https : http).request(options, response => {
    if (response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
      response.resume();
      return fetchUrl(new URL(response.headers.location, address).toString(), depth + 1);
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      response.resume();
      return fail('HTTP ' + response.statusCode);
    }
    const stream = fs.createWriteStream(output);
    let bytes = 0;
    response.on('data', chunk => { bytes += chunk.length; });
    response.pipe(stream);
    stream.on('finish', () => { process.stdout.write(String(bytes)); });
  });
  request.setTimeout(30000, () => request.destroy(new Error('request timeout')));
  request.on('error', fail);
  request.end();
}
if (!target || !output) fail('missing arguments'); else fetchUrl(target, 0);
)JS";
        std::ofstream stream(script, std::ios::binary | std::ios::trunc);
        stream.write(script_source, static_cast<std::streamsize>(sizeof(script_source) - 1));
        stream.close();
    }
    const auto output = temp / ("proxy-fetch-" + std::to_string(GetCurrentProcessId()) + ".bin");
    std::filesystem::remove(output, filesystem_error);
    std::string command = "set \"NODE_PATH=" + path_utf8(node_modules) + "\" && ";
    if (const auto proxy = local_http_proxy()) {
        command += "set \"HTTPS_PROXY=" + *proxy + "\" && set \"HTTP_PROXY=" + *proxy + "\" && ";
    }
    command += node + " \"" + path_utf8(script) + "\" \"" + url + "\" \"" +
               path_utf8(output) + "\"";
    const auto result = capture(command, limit <= 4 * 1024 * 1024 ? 45000 : 5 * 60 * 1000,
                                512ULL * 1024 * 1024, cancel);
    if (cancel && cancel->load()) {
        error = "用户已取消下载。";
        std::filesystem::remove(output, filesystem_error);
        return false;
    }
    if (!std::filesystem::exists(output)) {
        error = result.output.empty() ? "Node.js 代理下载失败。" : result.output;
        return false;
    }
    const auto size = std::filesystem::file_size(output, filesystem_error);
    if (filesystem_error || size == 0 || size > limit) {
        error = size > limit ? "下载内容超过安全大小限制。" : "Node.js 代理没有返回有效内容。";
        std::filesystem::remove(output, filesystem_error);
        return false;
    }
    std::ifstream stream(output, std::ios::binary);
    body.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    std::filesystem::remove(output, filesystem_error);
    if (body.empty()) {
        error = "Node.js 代理下载内容为空。";
        return false;
    }
    return true;
}

}  // namespace

std::filesystem::path state_directory() {
    wchar_t* value = nullptr;
    std::size_t length{};
    if (_wdupenv_s(&value, &length, L"LOCALAPPDATA") == 0 && value) {
        const std::filesystem::path result = std::filesystem::path(value) / "DshLauncher";
        free(value);
        return result;
    }
    if (value) free(value);
    return std::filesystem::temp_directory_path() / "DshLauncher";
}

std::optional<std::string> find_dsh() {
    {
        std::ifstream stream(remembered_dsh_file());
        std::string remembered;
        std::getline(stream, remembered);
        if (!remembered.empty() && std::filesystem::exists(std::filesystem::path(utf8_to_wide(remembered)))) {
            return remembered;
        }
    }
    const auto managed = managed_dsh_command();
    if (std::filesystem::exists(managed)) {
        remember_dsh(managed);
        return path_utf8(managed);
    }
    const auto result = capture("where.exe dsh 2>nul");
    const auto path = first_line(result.output);
    if (!path.empty()) remember_dsh(std::filesystem::path(utf8_to_wide(path)));
    return path.empty() ? std::nullopt : std::optional<std::string>(path);
}

bool has_node() {
    if (std::filesystem::exists(preferred_node_directory() / "node.exe")) return true;
    return capture("where.exe node.exe 2>nul").exit_code == 0;
}

bool has_npm() {
    if (std::filesystem::exists(preferred_node_directory() / "npm.cmd")) return true;
    return capture("where.exe npm.cmd 2>nul").exit_code == 0;
}

std::string node_version() {
    return first_line(capture(executable_command(preferred_node_directory() / "node.exe", "node.exe") + " --version 2>nul").output);
}

std::string npm_version() {
    return first_line(capture(executable_command(preferred_node_directory() / "npm.cmd", "npm.cmd") + " --version 2>nul").output);
}

std::filesystem::path default_dsh_directory() { return runtime_directory() / "dsh"; }

bool install_managed_node(bool official_source, std::string& error,
                          const std::atomic_bool* cancel) {
    if (!has_update_memory_budget(error)) return false;
    const auto runtime = runtime_directory();
    const auto target = managed_node_directory();
    const auto staging = runtime / "node-installing";
    std::error_code filesystem_error;
    std::filesystem::create_directories(runtime, filesystem_error);
    std::filesystem::remove_all(staging, filesystem_error);
    std::filesystem::create_directories(staging, filesystem_error);

    std::vector<unsigned char> index_data;
    const auto official_index = std::string("https://nodejs.org/dist/index.json");
    const auto mirror_index = std::string("https://npmmirror.com/mirrors/node/index.json");
    if (official_source) {
        if (!download(official_index, index_data, error, 4 * 1024 * 1024, cancel)) return false;
    } else if (!download(mirror_index, index_data, error, 4 * 1024 * 1024, cancel) &&
               !(cancel && cancel->load()) &&
               !download(official_index, index_data, error, 4 * 1024 * 1024, cancel)) {
        return false;
    }
    const std::string index(index_data.begin(), index_data.end());
    const std::regex release_pattern(
        R"REGEX(\{[^{}]*"version"\s*:\s*"(v[0-9.]+)"[^{}]*"lts"\s*:\s*(?!false|null)[^,}]+[^{}]*"files"\s*:\s*\[[^]]*"win-x64-zip")REGEX");
    std::smatch release_match;
    if (!std::regex_search(index, release_match, release_pattern)) {
        error = "没有找到兼容的 Node.js LTS x64 版本。";
        return false;
    }
    const auto version = release_match[1].str();
    const auto official_archive = "https://nodejs.org/dist/" + version + "/node-" + version + "-win-x64.zip";
    const auto mirror_archive = "https://npmmirror.com/mirrors/node/" + version + "/node-" + version + "-win-x64.zip";
    std::vector<unsigned char> archive_data;
    if (official_source) {
        if (!download(official_archive, archive_data, error, 128 * 1024 * 1024, cancel)) return false;
    } else if (!download(mirror_archive, archive_data, error, 128 * 1024 * 1024, cancel) &&
               !(cancel && cancel->load()) &&
               !download(official_archive, archive_data, error, 128 * 1024 * 1024, cancel)) {
        return false;
    }
    const auto archive = staging / "node.zip";
    if (!write_binary(archive, archive_data)) {
        error = "无法保存 Node.js 安装包。";
        return false;
    }
    const auto result = capture("tar.exe -xf \"" + path_utf8(archive) + "\" -C \"" +
                                path_utf8(staging) + "\"", 5 * 60 * 1000,
                                512ULL * 1024 * 1024, cancel);
    std::filesystem::path extracted;
    for (const auto& entry : std::filesystem::directory_iterator(staging, filesystem_error)) {
        if (entry.is_directory()) { extracted = entry.path(); break; }
    }
    if (result.exit_code == 0 && !extracted.empty()) {
        std::filesystem::remove_all(target, filesystem_error);
        filesystem_error.clear();
        std::filesystem::create_directories(target, filesystem_error);
        filesystem_error.clear();
        std::filesystem::copy(extracted, target,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              filesystem_error);
    }
    std::filesystem::remove_all(staging, filesystem_error);
    if (result.exit_code != 0 || filesystem_error || !std::filesystem::exists(target / "node.exe") ||
        !std::filesystem::exists(target / "npm.cmd")) {
        error = result.output.empty() ? "Node.js 下载或解压失败，请检查网络后重试。" : result.output;
        return false;
    }
    return true;
}

bool has_managed_node() {
    return std::filesystem::exists(managed_node_directory() / "node.exe");
}

bool install_managed_node(std::string& error) { return install_managed_node(false, error, nullptr); }

bool install_system_node(bool official_source, const std::filesystem::path& directory,
                         std::string& error, const std::atomic_bool* cancel) {
    if (directory.empty()) {
        error = "Node.js 安装目录不能为空。";
        return false;
    }
    if (cancel && cancel->load()) {
        error = "用户已取消 Node.js 安装。";
        return false;
    }
    const auto target = directory.lexically_normal();
    std::error_code filesystem_error;
    std::filesystem::create_directories(state_directory() / "downloads", filesystem_error);
    if (filesystem_error) {
        error = "无法创建 Node.js 下载缓存目录。";
        return false;
    }
    if (official_source) {
        if (capture("where.exe winget.exe 2>nul", 5000).exit_code != 0) {
            error = "未检测到 winget，无法使用 Node.js 官方源；请选择国内镜像，或先安装 App Installer。";
            return false;
        }
        const std::wstring params = L"/d /s /c \"winget install -e --id OpenJS.NodeJS.LTS "
            L"--accept-source-agreements --accept-package-agreements --location \"" +
            target.wstring() + L"\"\"";
        if (!run_elevated_and_wait(L"cmd.exe", params, error, cancel)) return false;
    } else {
        std::vector<unsigned char> index_data;
        if (!download("https://npmmirror.com/mirrors/node/index.json", index_data, error,
                      4 * 1024 * 1024, cancel)) {
            return false;
        }
        const std::string index(index_data.begin(), index_data.end());
        static const std::regex lts_msi_pattern(
            R"REGEX(\{[^{}]*"version"\s*:\s*"(v[0-9.]+)"[^{}]*"lts"\s*:\s*(?!false|null)[^,}]+[^{}]*"files"\s*:\s*\[[^]]*"win-x64-msi")REGEX");
        std::smatch match;
        if (!std::regex_search(index, match, lts_msi_pattern)) {
            error = "国内镜像未返回可用的 Node.js LTS x64 MSI 信息。";
            return false;
        }
        const auto version = match[1].str();
        const auto installer = state_directory() / "downloads" / "node-lts-x64.msi";
        std::vector<unsigned char> installer_data;
        const auto url = "https://npmmirror.com/mirrors/node/" + version + "/node-" +
                         version + "-x64.msi";
        if (!download(url, installer_data, error, 96 * 1024 * 1024, cancel) ||
            !write_binary(installer, installer_data)) {
            if (error.empty()) error = "无法保存 Node.js 镜像安装包。";
            return false;
        }
        const std::wstring params = L"/i \"" + installer.wstring() + L"\" INSTALLDIR=\"" +
                                    target.wstring() + L"\" /qb /norestart";
        const bool installed = run_elevated_and_wait(L"msiexec.exe", params, error, cancel);
        std::filesystem::remove(installer, filesystem_error);
        if (!installed) return false;
    }
    if (!std::filesystem::exists(target / "node.exe") || !std::filesystem::exists(target / "npm.cmd")) {
        error = "Node.js 安装器已结束，但所选目录中未找到 node.exe 和 npm.cmd：" + path_utf8(target);
        return false;
    }
    remember_node_directory(target);
    remember_launcher_owned_node(target);
    return true;
}

bool install_dsh_at(const std::filesystem::path& prefix, bool official_source, std::string& error,
                    const std::string& requested_version,
                    const std::atomic_bool* cancel,
                    const std::function<void(const std::string&)>& progress) {
    if (prefix.empty()) {
        error = "DSH 安装目录不能为空。";
        return false;
    }
    if (!has_update_memory_budget(error)) return false;
    const auto update_lock = CreateMutexW(nullptr, TRUE, L"Local\\DshLauncher.DshInstall");
    if (!update_lock || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (update_lock) CloseHandle(update_lock);
        error = "已有另一个 DSH 安装或更新任务正在运行。";
        return false;
    }
    struct MutexGuard {
        HANDLE value{};
        ~MutexGuard() { if (value) CloseHandle(value); }
    } update_lock_guard{update_lock};
    std::error_code filesystem_error;
    std::filesystem::create_directories(prefix, filesystem_error);
    if (filesystem_error) {
        error = "无法创建 DSH 安装目录。";
        return false;
    }
    const auto node_directory = preferred_node_directory();
    const auto npm = executable_command(node_directory / "npm.cmd", "npm.cmd");
    const auto node_path = path_utf8(node_directory);
    const auto prefix_path = path_utf8(prefix);
    // Keep npm peer dependency resolution enabled. DSH's boot package requires
    // peer plugins that are omitted when --legacy-peer-deps is used.
    const auto package = requested_version.empty() ? std::string("@deepseek-ai/dsh")
                                                   : "@deepseek-ai/dsh@" + requested_version;
    const std::string options = " install --prefix \"" + prefix_path +
                                "\" " + package +
                                " --no-fund --no-audit";
    // The original BAT carries an --allow-scripts allow-list. npm 11.9.0
    // reports that option as unknown, while normal npm installs already run
    // package scripts by default. Do not pass a legacy no-op flag that merely
    // emits a warning and creates a misleading extra retry.
    const auto report = [&progress](const std::string& message) {
        if (progress) progress(message);
    };
    std::vector<const char*> registries;
    std::vector<std::string> probe_diagnostics;
    const auto probe_registry = [&](const char* registry) {
        if (cancel && cancel->load()) return false;
        report(std::string("正在检查 npm 源：") + registry);
        std::vector<unsigned char> body;
        std::string probe_error;
        if (download(std::string(registry) + "/@deepseek-ai%2fdsh/latest", body, probe_error,
                     4 * 1024 * 1024, cancel)) {
            report(std::string("npm 源元数据连接成功：") + registry);
            return true;
        }
        if (cancel && cancel->load()) return false;
        std::string npm_diagnostic;
        if (npm_query_version(registry, cancel, &npm_diagnostic)) {
            report(std::string("npm 命令回退连接成功：") + registry);
            return true;
        }
        const auto diagnostic = std::string(registry) + " => " +
            (probe_error.empty() ? (npm_diagnostic.empty() ? "没有可用响应" : npm_diagnostic)
                                 : probe_error + (npm_diagnostic.empty() ? "" : "; " + npm_diagnostic));
        probe_diagnostics.push_back(diagnostic);
        report(std::string("npm 源不可用：") + diagnostic);
        return false;
    };
    if (official_source) {
        if (probe_registry(official_npm_registry)) registries.push_back(official_npm_registry);
    } else {
        for (const auto* registry : domestic_npm_registries) {
            if (cancel && cancel->load()) break;
            if (probe_registry(registry)) {
                registries.push_back(registry);
            }
        }
        if (!(cancel && cancel->load()) && probe_registry(official_npm_registry)) {
            registries.push_back(official_npm_registry);
        }
    }
    if (registries.empty()) {
        error = cancel && cancel->load()
                    ? "用户已取消网络检查。"
                    : "无法连接 npm 更新源（国内镜像和官方源均不可达），未启动安装任务；请检查 HTTPS 网络、系统代理或 TUN 设置。";
        if (!probe_diagnostics.empty()) {
            error += "\n诊断：";
            for (const auto& diagnostic : probe_diagnostics) error += "\n- " + diagnostic;
        }
        return false;
    }
    // DSH has a large peer graph. Keep npm below a dynamic ceiling while
    // reserving memory for Windows. The former fixed 1 GiB V8/2 GiB Job cap
    // killed npm halfway through idealTree on this dependency graph.
    auto npm_cache = state_directory() / "npm-cache";
    std::filesystem::create_directories(npm_cache, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(npm_cache)) {
        filesystem_error.clear();
        npm_cache = std::filesystem::temp_directory_path() / "DshLauncher" / "npm-cache";
        std::filesystem::create_directories(npm_cache, filesystem_error);
    }
    const auto proxy = local_http_proxy();
    const auto memory_policy = npm_memory_policy();
    const auto make_command = [&](const char* registry, bool official_fallback) {
        std::string command = "set \"PATH=" + node_path + ";%PATH%\" && "
               "set \"npm_config_cache=" + path_utf8(npm_cache) + "\" && "
               "set \"NODE_OPTIONS=--max-old-space-size=" + std::to_string(memory_policy.v8_heap_mib) + "\" && "
               "set \"npm_config_maxsockets=4\" && "
               "set \"npm_config_foreground_scripts=true\" && ";
        if (proxy) {
            command += "set \"HTTP_PROXY=" + *proxy + "\" && set \"HTTPS_PROXY=" + *proxy + "\" && ";
        }
        command += npm + options;
        // The BAT only adds explicit fetch limits for its final official
        // fallback.  Keep the domestic command byte-for-byte equivalent in
        // behavior, while bounding an unreachable official source.
        if (official_fallback) command += " --fetch-timeout=45000 --fetch-retries=0";
        command += " --registry=" + std::string(registry);
        return command;
    };
    // npm can fan out many Node workers while resolving DSH's peer graph.
    // Keep the update isolated from the rest of the desktop: if the whole
    // install job exceeds 2 GiB, terminate that job and report failure.
    const auto mib = [](std::size_t bytes) { return bytes / (1024ULL * 1024ULL); };
    const auto summarize = [&](const char* registry, const CommandResult& result) {
        std::string text = std::string("npm 尝试结束：源=") + registry +
                           "; 模式=BAT 标准安装" +
                           "; 耗时=" + std::to_string(result.elapsed_ms / 1000) + " 秒" +
                           "; 退出码=" + std::to_string(result.exit_code) +
                           "; Job 峰值内存=" + std::to_string(mib(result.peak_job_memory)) + " MiB" +
                           "; 限制=" + std::to_string(mib(result.job_memory_limit)) + " MiB";
        if (result.cancelled) text += "; 已取消";
        if (result.timed_out) text += "; 已超时";
        return text;
    };
    std::vector<std::string> attempt_summaries;
    report("npm 内存保护：V8 上限=" + std::to_string(memory_policy.v8_heap_mib) +
           " MiB；任务组上限=" + std::to_string(mib(memory_policy.job_limit)) + " MiB");
    const auto run_attempt = [&](const char* registry, bool official_fallback) {
        report(std::string("正在通过 ") + registry + " 更新 DSH（BAT 标准安装）");
        // DSH 0.1.1's npm dependency graph can spend more than five minutes
        // in idealTree before downloading packages. The original BAT lets that
        // command finish. A 15-minute wall clock limit still bounds a broken
        // task, while cancellation, per-fetch limits and the Job memory limit
        // keep it safe and the heartbeat keeps the UI observable.
        const auto result = capture(make_command(registry, official_fallback), 15 * 60 * 1000,
                                    memory_policy.job_limit, cancel, report);
        const auto summary = summarize(registry, result);
        attempt_summaries.push_back(summary);
        report(summary);
        return result;
    };
    CommandResult result;
    bool installed_ok = false;
    for (const auto* registry : registries) {
        const bool official_fallback = std::string(registry) == official_npm_registry;
        result = run_attempt(registry, official_fallback);
        if (result.exit_code == 0) { installed_ok = true; break; }
        if (result.cancelled) break;
    }
    // npm installs package executables into <prefix>\\node_modules\\.bin
    // when using --prefix (without -g). Keep the exact shim path because it
    // contains the correct relative package path and works for custom dirs.
    auto installed = prefix / "node_modules" / ".bin" / "dsh.cmd";
    if (!std::filesystem::exists(installed)) installed = prefix / "dsh.cmd";
    if (!installed_ok || !std::filesystem::exists(installed)) {
        error = result.output.empty() ? "DSH 下载失败。" : result.output;
        if (!attempt_summaries.empty()) {
            error += "\n诊断：";
            for (const auto& summary : attempt_summaries) error += "\n- " + summary;
        }
        return false;
    }
    remember_dsh(installed);
    return true;
}

bool install_managed_dsh(std::string& error) { return install_dsh_at(default_dsh_directory(), false, error); }

bool uninstall_dsh(std::string& error) {
    std::ifstream stream(remembered_dsh_file());
    std::string remembered_text;
    std::getline(stream, remembered_text);
    if (remembered_text.empty()) {
        error = "没有找到启动器记录的 DSH 安装目录。";
        return false;
    }
    const auto executable = std::filesystem::path(utf8_to_wide(remembered_text));
    auto prefix = executable.parent_path();
    if (prefix.filename() == L".bin" && prefix.parent_path().filename() == L"node_modules") {
        prefix = prefix.parent_path().parent_path();
    }
    std::error_code filesystem_error;
    if (prefix == default_dsh_directory()) {
        std::filesystem::remove_all(prefix, filesystem_error);
    } else {
        std::filesystem::remove_all(prefix / "node_modules" / "@deepseek-ai" / "dsh", filesystem_error);
        filesystem_error.clear();
        for (const auto& name : {"dsh.cmd", "dsh.ps1", "dsh"}) {
            std::filesystem::remove(prefix / "node_modules" / ".bin" / name, filesystem_error);
            filesystem_error.clear();
        }
    }
    if (filesystem_error) {
        error = "删除 DSH 文件失败：" + filesystem_error.message();
        return false;
    }
    if (std::filesystem::exists(executable)) {
        error = "卸载后仍检测到 DSH 可执行文件，可能被其他程序占用。";
        return false;
    }
    clear_remembered_dsh();
    return true;
}

bool clear_conversation_memory(std::string& error) {
    const auto home = dsh_home_directory();
    if (home.empty() || home == home.root_path()) {
        error = "无法确定安全的 DSH_HOME 目录，已拒绝清除对话记忆。";
        return false;
    }
    std::error_code filesystem_error;
    for (const auto* name : {L"sessions", L"storages"}) {
        const auto target = home / name;
        std::filesystem::remove_all(target, filesystem_error);
        if (filesystem_error) {
            error = "清除 DSH " + path_utf8(target) + " 失败：" + filesystem_error.message();
            return false;
        }
    }
    return true;
}

bool uninstall_managed_node(std::string& error) {
    const auto target = managed_node_directory();
    if (!std::filesystem::exists(target)) return true;
    std::error_code filesystem_error;
    std::filesystem::remove_all(target, filesystem_error);
    if (filesystem_error || std::filesystem::exists(target)) {
        error = "删除启动器托管的 Node.js 失败";
        if (filesystem_error) error += "：" + filesystem_error.message();
        return false;
    }
    return true;
}

bool has_launcher_owned_node() {
    if (has_managed_node()) return true;
    const auto owned = launcher_owned_node_directory();
    return !owned.empty() && std::filesystem::exists(owned / "node.exe");
}

bool uninstall_launcher_owned_node(std::string& error) {
    if (has_managed_node()) return uninstall_managed_node(error);
    const auto owned = launcher_owned_node_directory();
    if (owned.empty()) {
        error = "未找到由启动器安装的 Node.js 记录；为保护用户环境，已拒绝卸载。";
        return false;
    }
    if (!std::filesystem::exists(owned / "node.exe")) {
        clear_launcher_owned_node();
        return true;
    }
    if (capture("where.exe winget.exe 2>nul", 5000).exit_code != 0) {
        error = "该 Node.js 由启动器安装，但系统缺少 winget，无法安全定位卸载器；请在 Windows“已安装的应用”中卸载 Node.js。";
        return false;
    }
    const std::wstring parameters = L"/d /s /c \"winget uninstall -e --id OpenJS.NodeJS.LTS "
                                    L"--accept-source-agreements\"";
    if (!run_elevated_and_wait(L"cmd.exe", parameters, error, nullptr)) return false;
    if (std::filesystem::exists(owned / "node.exe")) {
        error = "Node.js 卸载器已结束，但安装目录仍存在。为保护其他程序，未删除该目录。";
        return false;
    }
    clear_launcher_owned_node();
    std::error_code filesystem_error;
    std::filesystem::remove(remembered_node_file(), filesystem_error);
    return true;
}

std::string dsh_version(const std::string& executable) {
    const auto node_path = path_utf8(managed_node_directory());
    return first_line(capture("set \"PATH=" + node_path + ";%PATH%\" && \"" + executable + "\" --version 2>nul").output);
}

std::optional<std::string> latest_dsh_version(bool official_source, const std::atomic_bool* cancel,
                                              std::string* diagnostic) {
    const auto query = [cancel, diagnostic](const char* registry) {
        if (cancel && cancel->load()) return std::optional<std::string>{};
        // npm uses Node's OpenSSL stack and can use a local Clash/Verge HTTP
        // proxy even when WinHTTP/Schannel cannot complete that proxy's TLS
        // handshake. Prefer it for metadata when a proxy is actually open.
        if (local_http_proxy()) {
            if (const auto version = npm_query_version(registry, cancel, diagnostic)) return version;
            if (cancel && cancel->load()) return std::optional<std::string>{};
        }
        std::vector<unsigned char> body;
        std::string error;
        const auto url = std::string(registry) + "/@deepseek-ai%2fdsh/latest";
        if (!download(url, body, error, 4 * 1024 * 1024, cancel)) {
            if (cancel && cancel->load()) return std::optional<std::string>{};
            if (const auto version = npm_query_version(registry, cancel, diagnostic)) return version;
            if (diagnostic) {
                if (!diagnostic->empty()) *diagnostic += "; ";
                *diagnostic += std::string(registry) + " => " + error;
            }
            return std::optional<std::string>{};
        }
        const auto version = extract_version(std::string(body.begin(), body.end()));
        if (!version && diagnostic) {
            if (!diagnostic->empty()) *diagnostic += "; ";
            *diagnostic += std::string(registry) + " => 返回内容没有可识别版本号";
        }
        return version;
    };
    if (official_source) return query(official_npm_registry);
    for (const auto* registry : domestic_npm_registries) {
        if (cancel && cancel->load()) return std::nullopt;
        if (const auto version = query(registry)) return version;
    }
    if (cancel && cancel->load()) return std::nullopt;
    return query(official_npm_registry);
}

std::optional<LauncherUpdate> latest_launcher_update(bool official_source,
                                                     const std::atomic_bool* cancel,
                                                     std::string* diagnostic) {
    const auto read_string = [](const std::string& object, const char* key) -> std::optional<std::string> {
        const std::regex pattern(std::string("\\\"") + key + R"(\"\s*:\s*\"([^\"]*)\")");
        std::smatch match;
        if (std::regex_search(object, match, pattern)) return match[1].str();
        return std::nullopt;
    };
    const auto read_number = [](const std::string& object, const char* key) -> std::optional<std::uint64_t> {
        const std::regex pattern(std::string("\\\"") + key + R"(\"\s*:\s*([0-9]+))");
        std::smatch match;
        if (!std::regex_search(object, match, pattern)) return std::nullopt;
        try {
            return static_cast<std::uint64_t>(std::stoull(match[1].str()));
        } catch (...) {
            return std::nullopt;
        }
    };
    const auto valid_sha256 = [](const std::string& value) {
        if (value.size() != 64 || value == "REPLACE_WITH_LOWERCASE_SHA256") return false;
        return std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isxdigit(character) != 0;
        });
    };
    const auto parse_manifest = [&](const std::string& text) -> std::optional<LauncherUpdate> {
        const auto version = read_string(text, "version");
        const auto marker = text.find("\"windows-x64\"");
        if (!version || marker == std::string::npos) return std::nullopt;
        const auto open = text.find('{', marker);
        const auto close = open == std::string::npos ? std::string::npos : text.find('}', open);
        if (open == std::string::npos || close == std::string::npos) return std::nullopt;
        const auto package = text.substr(open, close - open + 1);
        const auto url = read_string(package, "url");
        const auto fallback = read_string(package, "fallbackUrl");
        const auto size = read_number(package, "size");
        const auto sha256 = read_string(package, "sha256");
        if (!url || !size || *size == 0 || !sha256 || !valid_sha256(*sha256) ||
            (url->rfind("https://", 0) != 0 && url->rfind("http://", 0) != 0)) {
            return std::nullopt;
        }
        LauncherUpdate result;
        result.version = *version;
        result.url = *url;
        result.fallback_url = fallback.value_or("");
        result.size = *size;
        result.sha256 = *sha256;
        return result;
    };

    const std::array<const char*, 2> manifests = official_source
        ? std::array<const char*, 2>{launcher_manifest_github, launcher_manifest_gitee}
        : std::array<const char*, 2>{launcher_manifest_gitee, launcher_manifest_github};
    for (const auto* manifest_url : manifests) {
        if (cancel && cancel->load()) {
            if (diagnostic) *diagnostic = "用户已取消启动器更新检查";
            return std::nullopt;
        }
        std::vector<unsigned char> body;
        std::string error;
        if (local_http_proxy()) {
            std::string node_error;
            if (node_download(manifest_url, body, node_error, 2 * 1024 * 1024, cancel)) {
                if (const auto result = parse_manifest(std::string(body.begin(), body.end()))) return result;
                if (diagnostic) {
                    if (!diagnostic->empty()) *diagnostic += "; ";
                    *diagnostic += std::string(manifest_url) + " => 清单缺少有效 windows-x64 包或 SHA-256";
                }
                continue;
            }
            if (cancel && cancel->load()) {
                if (diagnostic) *diagnostic = "用户已取消启动器更新检查";
                return std::nullopt;
            }
            if (diagnostic && !node_error.empty()) {
                if (!diagnostic->empty()) *diagnostic += "; ";
                *diagnostic += std::string(manifest_url) + " => Node.js 代理回退失败：" + node_error;
            }
            if (node_error.rfind("HTTP 404", 0) == 0 || node_error.rfind("HTTP 403", 0) == 0) {
                // A reachable host returning 404/403 is a repository/configuration
                // problem, not a transport problem; do not repeat a slower
                // WinHTTP/TLS probe for the same URL.
                continue;
            }
        }
        body.clear();
        if (!download(manifest_url, body, error, 2 * 1024 * 1024, cancel)) {
            if (cancel && cancel->load()) {
                if (diagnostic) *diagnostic = "用户已取消启动器更新检查";
                return std::nullopt;
            }
            if (diagnostic) {
                if (!diagnostic->empty()) *diagnostic += "; ";
                *diagnostic += std::string(manifest_url) + " => " + error;
            }
            continue;
        }
        if (const auto result = parse_manifest(std::string(body.begin(), body.end()))) return result;
        if (diagnostic) {
            if (!diagnostic->empty()) *diagnostic += "; ";
            *diagnostic += std::string(manifest_url) + " => 清单缺少有效 windows-x64 包或 SHA-256";
        }
    }
    return std::nullopt;
}

bool update_launcher(const LauncherUpdate& update, std::string& error,
                     const std::atomic_bool* cancel) {
    if (update.url.empty() || update.size == 0 || update.sha256.empty()) {
        error = "启动器更新清单不完整，已拒绝下载。";
        return false;
    }
    if (update.size > 512ULL * 1024 * 1024) {
        error = "启动器更新包超过安全大小限制，已拒绝下载。";
        return false;
    }
    if (cancel && cancel->load()) {
        error = "用户已取消启动器更新。";
        return false;
    }
    std::error_code filesystem_error;
    const auto updates = state_directory() / "updates";
    const auto staging = updates / ("launcher-" + update.version);
    std::filesystem::create_directories(updates, filesystem_error);
    if (filesystem_error) {
        error = "无法创建启动器更新目录：" + filesystem_error.message();
        return false;
    }
    std::filesystem::remove_all(staging, filesystem_error);
    std::filesystem::create_directories(staging, filesystem_error);
    if (filesystem_error) {
        error = "无法创建启动器更新临时目录：" + filesystem_error.message();
        return false;
    }
    const auto archive = updates / ("launcher-" + update.version + ".zip");
    std::vector<unsigned char> body;
    std::string download_error;
    const auto limit = static_cast<std::size_t>((std::min)(update.size + 4ULL * 1024 * 1024,
                                                            512ULL * 1024 * 1024));
    if (local_http_proxy() && !(cancel && cancel->load())) {
        std::string node_error;
        if (!node_download(update.url, body, node_error, limit, cancel) &&
            !update.fallback_url.empty() && !(cancel && cancel->load())) {
            node_download(update.fallback_url, body, node_error, limit, cancel);
        }
        if (body.empty() && !node_error.empty()) download_error = node_error;
    }
    if (body.empty() && !(cancel && cancel->load())) {
        if (!download(update.url, body, download_error, limit, cancel) &&
            !(cancel && cancel->load()) && !update.fallback_url.empty()) {
            download_error.clear();
            download(update.fallback_url, body, download_error, limit, cancel);
        }
    }
    if (cancel && cancel->load()) {
        error = "用户已取消启动器更新。";
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    if (body.empty()) {
        error = download_error.empty() ? "启动器更新包下载失败。" : download_error;
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    if (body.size() != update.size) {
        error = "启动器更新包大小校验失败：期望 " + std::to_string(update.size) +
                " 字节，实际 " + std::to_string(body.size()) + " 字节。";
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    if (!write_binary(archive, body)) {
        error = "无法保存启动器更新包。";
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    const auto hash_result = capture("certutil.exe -hashfile \"" + path_utf8(archive) +
                                     "\" SHA256 2>nul", 30000, 64ULL * 1024 * 1024, cancel);
    std::smatch hash_match;
    const std::regex hash_pattern(R"(([A-Fa-f0-9]{64}))");
    if (!std::regex_search(hash_result.output, hash_match, hash_pattern)) {
        error = "无法完成启动器更新包 SHA-256 校验（系统缺少 certutil 或输出异常）。";
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    std::string actual_hash = hash_match[1].str();
    std::transform(actual_hash.begin(), actual_hash.end(), actual_hash.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::string expected_hash = update.sha256;
    std::transform(expected_hash.begin(), expected_hash.end(), expected_hash.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (actual_hash != expected_hash) {
        error = "启动器更新包 SHA-256 校验失败。";
        std::filesystem::remove(archive, filesystem_error);
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    const auto extract_result = capture("tar.exe -xf \"" + path_utf8(archive) + "\" -C \"" +
                                       path_utf8(staging) + "\"", 120000,
                                       512ULL * 1024 * 1024, cancel);
    if (extract_result.exit_code != 0) {
        error = extract_result.output.empty() ? "启动器更新包解压失败。" : extract_result.output;
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    std::filesystem::path replacement;
    for (std::filesystem::recursive_directory_iterator iterator(staging, filesystem_error), end;
         iterator != end && !filesystem_error; iterator.increment(filesystem_error)) {
        if (iterator->is_regular_file(filesystem_error) &&
            iterator->path().filename() == L"dsh-launcher.exe") {
            replacement = iterator->path();
            break;
        }
    }
    if (replacement.empty()) {
        error = "更新包中没有找到 dsh-launcher.exe。";
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    wchar_t executable_buffer[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable_buffer,
                                            static_cast<DWORD>(std::size(executable_buffer)));
    if (length == 0 || length >= std::size(executable_buffer)) {
        error = "无法定位当前启动器文件。";
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    const std::filesystem::path current(executable_buffer, executable_buffer + length);
    const auto script = updates / ("apply-launcher-" + std::to_string(GetCurrentProcessId()) + ".cmd");
    std::ofstream script_stream(script, std::ios::binary | std::ios::trunc);
    if (!script_stream) {
        error = "无法创建启动器更新助手。";
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    script_stream << "@echo off\r\n"
                  << "setlocal\r\n"
                  << "for /l %%N in (1,1,30) do (\r\n"
                  << "  copy /Y \"" << path_utf8(replacement) << "\" \"" << path_utf8(current) << "\" >nul 2>&1\r\n"
                  << "  if not errorlevel 1 goto start\r\n"
                  << "  timeout /t 1 /nobreak >nul\r\n"
                  << ")\r\n"
                  << "exit /b 1\r\n"
                  << ":start\r\n"
                  << "start \"\" \"" << path_utf8(current) << "\"\r\n"
                  << "del \"%~f0\"\r\n";
    script_stream.close();
    if (!script_stream) {
        error = "无法写入启动器更新助手。";
        std::filesystem::remove(script, filesystem_error);
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    const auto command = std::wstring(L"cmd.exe /d /s /c \"\"") + script.wstring() + L"\"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                        &startup, &process)) {
        error = "无法启动启动器更新助手。";
        std::filesystem::remove(script, filesystem_error);
        std::filesystem::remove_all(staging, filesystem_error);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool is_web_running() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(3080);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    // A blocking localhost connect can still wait for Windows' TCP retry
    // timeout (often 2+ minutes) when a firewall/filter drops the SYN.  That
    // turned a 30-second startup health check into the multi-minute hang seen
    // on the Windows 10 test machine.  Health probing must be bounded.
    u_long non_blocking = 1;
    bool connected = false;
    if (ioctlsocket(socket_handle, FIONBIO, &non_blocking) == 0) {
        const int connect_result = connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (connect_result == 0) {
            connected = true;
        } else {
            const int socket_error = WSAGetLastError();
            if (socket_error == WSAEWOULDBLOCK || socket_error == WSAEINPROGRESS ||
                socket_error == WSAEALREADY) {
                fd_set writable{};
                FD_SET(socket_handle, &writable);
                timeval timeout{};
                timeout.tv_sec = 0;
                timeout.tv_usec = 200000;
                if (select(0, nullptr, &writable, nullptr, &timeout) > 0 && FD_ISSET(socket_handle, &writable)) {
                    int completion_error{};
                    int length = sizeof(completion_error);
                    connected = getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                                            reinterpret_cast<char*>(&completion_error), &length) == 0 &&
                                completion_error == 0;
                }
            }
        }
    }
    closesocket(socket_handle);
    WSACleanup();
    return connected;
}

std::optional<std::uint32_t> start_dsh(
    const std::string& executable,
    const std::filesystem::path& service_log,
    std::string& error) {
    std::filesystem::create_directories(service_log.parent_path());
    const auto wide_executable = utf8_to_wide(executable);
    const auto node_path = preferred_node_directory().wstring();
    // Keep the proven BAT service command exactly as `dsh web`.  The app layer
    // separately issues the BAT's delayed `start http://127.0.0.1:3080`.
    // Older DSH builds installed by the BAT do not all support `--no-open`;
    // passing it can make the service exit before 3080 is listening.  The
    // optional launcher-owned Node directory stays first in PATH for legacy
    // managed installations, without altering Node memory.
    const std::wstring path_setup = node_path.empty() ? L"" : L"set \"PATH=" + node_path + L";%PATH%\" && ";
    std::wstring command = L"cmd.exe /d /s /c \"" + path_setup + L"\"" + wide_executable +
                           L"\" web >> " + quote(service_log) + L" 2>&1\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &startup, &process)) {
        error = "CreateProcess 失败，错误码 " + std::to_string(GetLastError());
        return std::nullopt;
    }
    const auto pid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return pid;
}

bool stop_process_tree(std::uint32_t pid, std::string& error) {
    // taskkill itself is normally immediate. Do not let it inherit the 30 s
    // general command timeout: a blocked Windows process manager would make
    // the launcher look frozen compared with the original BAT.
    const auto result = capture("taskkill.exe /PID " + std::to_string(pid) + " /T /F",
                                5000, 64ULL * 1024 * 1024);
    if (result.exit_code != 0) {
        error = "taskkill 无法停止进程：" + result.output;
        return false;
    }
    return true;
}

bool open_web(std::string& error) {
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:3080", nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        error = "无法打开系统默认浏览器。";
        return false;
    }
    return true;
}

}  // namespace dsh::platform

#endif

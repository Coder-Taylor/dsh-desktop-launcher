#ifdef _WIN32

#include "platform/platform.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <vector>

namespace dsh::platform {
namespace {

std::atomic_bool use_official_source{};

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
};

CommandResult capture(const std::string& command, DWORD timeout_ms = 30000) {
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
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return {};
    }
    CloseHandle(write_pipe);
    CloseHandle(process.hThread);

    const auto job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, process.hProcess);
    }

    CommandResult result;
    std::array<char, 512> buffer{};
    const auto drain_output = [&] {
        for (;;) {
            DWORD available{};
            if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) || !available) break;
            DWORD bytes_read{};
            const auto wanted = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
            if (!ReadFile(read_pipe, buffer.data(), wanted, &bytes_read, nullptr) || !bytes_read) break;
            if (result.output.size() < 16384) {
                result.output.append(buffer.data(), std::min<std::size_t>(bytes_read, 16384 - result.output.size()));
            }
        }
    };
    const auto started = GetTickCount64();
    bool completed{};
    while (!completed) {
        drain_output();
        completed = WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0;
        if (!completed && GetTickCount64() - started >= timeout_ms) {
            if (job) TerminateJobObject(job, ERROR_TIMEOUT);
            else TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 5000);
            result.exit_code = ERROR_TIMEOUT;
            result.output += "\n命令执行超时，已安全终止。";
            completed = true;
        }
    }
    if (result.exit_code != ERROR_TIMEOUT) GetExitCodeProcess(process.hProcess, &result.exit_code);
    drain_output();
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
    static const std::regex pattern(R"((?:^|\s)v?(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)(?:\s|$))");
    std::smatch match;
    if (std::regex_search(output, match, pattern)) return match[1].str();
    return std::nullopt;
}

std::wstring quote(const std::filesystem::path& value) {
    return L"\"" + value.wstring() + L"\"";
}

std::filesystem::path runtime_directory() {
    return state_directory() / "runtime";
}

std::filesystem::path managed_node_directory() {
    return runtime_directory() / "node";
}

std::filesystem::path managed_dsh_command() {
    return runtime_directory() / "dsh" / "dsh.cmd";
}

std::string path_utf8(const std::filesystem::path& value);

std::filesystem::path remembered_dsh_file() {
    return state_directory() / "dsh-location.txt";
}

void remember_dsh(const std::filesystem::path& executable) {
    std::error_code error;
    std::filesystem::create_directories(state_directory(), error);
    std::ofstream stream(remembered_dsh_file(), std::ios::trunc);
    stream << path_utf8(executable);
}

std::string path_utf8(const std::filesystem::path& value) {
    const auto wide = value.wstring();
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string executable_command(const std::filesystem::path& managed, const char* fallback) {
    if (std::filesystem::exists(managed)) return "\"" + path_utf8(managed) + "\"";
    return fallback;
}

bool download(const std::string& url, std::vector<unsigned char>& body, std::string& error, std::size_t limit = 64 * 1024 * 1024) {
    const auto wide_url = utf8_to_wide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
        error = "更新地址格式无效。";
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const auto session = WinHttpOpen(L"DshLauncher/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "无法初始化网络组件。"; return false; }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 15000);
    const auto connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection) { WinHttpCloseHandle(session); error = "无法连接更新服务器。"; return false; }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const auto request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status{};
    DWORD status_size = sizeof(status);
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) &&
                 status >= 200 && status < 300;
    body.clear();
    while (ok) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request, &available)) { ok = false; break; }
        if (!available) break;
        if (body.size() + available > limit) { error = "下载文件超过安全大小限制。"; ok = false; break; }
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD read{};
        if (!WinHttpReadData(request, body.data() + offset, available, &read)) { ok = false; break; }
        body.resize(offset + read);
    }
    if (request) WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok || body.empty()) {
        if (error.empty()) error = "更新服务器没有返回有效内容。";
        return false;
    }
    return true;
}

std::string sha256(const std::vector<unsigned char>& data) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD object_length{}, hash_length{}, copied{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length), &copied, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length), &copied, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<unsigned char> object(object_length), digest(hash_length);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_length, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), hash_length, 0) < 0) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : digest) stream << std::setw(2) << static_cast<unsigned>(byte);
    return stream.str();
}

std::optional<std::string> json_string(const std::string& json, const std::string& key) {
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) return match[1].str();
    return std::nullopt;
}

std::filesystem::path current_executable() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
}

bool write_binary(const std::filesystem::path& path, const std::vector<unsigned char>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    return stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())).good();
}

}  // namespace

void set_official_update_source(bool official) { use_official_source.store(official); }

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
        if (!remembered.empty() && std::filesystem::exists(std::filesystem::path(utf8_to_wide(remembered)))) return remembered;
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
    if (std::filesystem::exists(managed_node_directory() / "node.exe")) return true;
    return capture("where.exe node.exe 2>nul").exit_code == 0;
}

bool has_npm() {
    if (std::filesystem::exists(managed_node_directory() / "npm.cmd")) return true;
    return capture("where.exe npm.cmd 2>nul").exit_code == 0;
}

std::string node_version() {
    return first_line(capture(executable_command(managed_node_directory() / "node.exe", "node.exe") + " --version 2>nul").output);
}

std::string npm_version() {
    return first_line(capture(executable_command(managed_node_directory() / "npm.cmd", "npm.cmd") + " --version 2>nul").output);
}

bool install_managed_node(std::string& error) {
    const auto runtime = runtime_directory();
    const auto target = managed_node_directory();
    const auto staging = runtime / "node-installing";
    std::error_code filesystem_error;
    std::filesystem::create_directories(runtime, filesystem_error);
    std::filesystem::remove_all(staging, filesystem_error);
    std::filesystem::create_directories(staging, filesystem_error);

    std::vector<unsigned char> index_data;
    const bool official = use_official_source.load();
    if (official) {
        if (!download("https://nodejs.org/dist/index.json", index_data, error, 4 * 1024 * 1024)) return false;
    } else if (!download("https://npmmirror.com/mirrors/node/index.json", index_data, error, 4 * 1024 * 1024) &&
               !download("https://nodejs.org/dist/index.json", index_data, error, 4 * 1024 * 1024)) {
        return false;
    }
    const std::string index(index_data.begin(), index_data.end());
    std::string version;
    std::size_t position{};
    const std::regex version_pattern(R"REGEX("version"\s*:\s*"(v[0-9.]+)")REGEX");
    std::smatch match;
    while (position < index.size()) {
        const auto tail = index.substr(position);
        if (!std::regex_search(tail, match, version_pattern)) break;
        const auto object_start = position + static_cast<std::size_t>(match.position());
        const auto object_end = index.find("}\n", object_start);
        const auto object = index.substr(object_start, object_end == std::string::npos ? std::string::npos : object_end - object_start);
        if (object.find("\"lts\":false") == std::string::npos && object.find("win-x64-zip") != std::string::npos) {
            version = match[1].str();
            break;
        }
        position = object_end == std::string::npos ? index.size() : object_end + 2;
    }
    if (version.empty()) { error = "没有找到兼容的 Node.js LTS 版本。"; return false; }
    std::vector<unsigned char> archive_data;
    const auto official_archive = "https://nodejs.org/dist/" + version + "/node-" + version + "-win-x64.zip";
    const auto mirror_archive = "https://npmmirror.com/mirrors/node/" + version + "/node-" + version + "-win-x64.zip";
    if (official) {
        if (!download(official_archive, archive_data, error)) return false;
    } else if (!download(mirror_archive, archive_data, error) && !download(official_archive, archive_data, error)) {
        return false;
    }
    const auto archive = staging / "node.zip";
    if (!write_binary(archive, archive_data)) { error = "无法保存 Node.js 安装包。"; return false; }
    const auto result = capture("tar.exe -xf \"" + path_utf8(archive) + "\" -C \"" + path_utf8(staging) + "\"");
    std::filesystem::path extracted;
    for (const auto& item : std::filesystem::directory_iterator(staging, filesystem_error)) {
        if (item.is_directory()) { extracted = item.path(); break; }
    }
    if (result.exit_code == 0 && !extracted.empty()) {
        std::filesystem::remove_all(target, filesystem_error);
        std::filesystem::create_directories(target, filesystem_error);
        std::filesystem::copy(extracted, target,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                              filesystem_error);
    }
    std::filesystem::remove_all(staging, filesystem_error);
    if (result.exit_code != 0 || !std::filesystem::exists(target / "node.exe") || !std::filesystem::exists(target / "npm.cmd")) {
        error = result.output.empty() ? "Node.js 下载或解压失败，请检查网络后重试。" : result.output;
        return false;
    }
    return true;
}

std::optional<LauncherUpdate> launcher_update_manifest(std::string& error) {
    constexpr const char* primary = "https://gitee.com/taylorchengitee/dsh-desktop-launcher/raw/main/packaging/manifests/update-manifest.json";
    constexpr const char* fallback = "https://raw.githubusercontent.com/Coder-Taylor/dsh-desktop-launcher/main/packaging/manifests/update-manifest.json";
    std::vector<unsigned char> data;
    if (use_official_source.load()) {
        if (!download(fallback, data, error, 256 * 1024)) return std::nullopt;
    } else if (!download(primary, data, error, 256 * 1024) && !download(fallback, data, error, 256 * 1024)) {
        return std::nullopt;
    }
    const std::string json(data.begin(), data.end());
    const auto package_marker = json.find("\"windows-x64\"");
    if (package_marker == std::string::npos) { error = "更新清单缺少 Windows x64 安装包。"; return std::nullopt; }
    const auto package_end = json.find('}', package_marker);
    const auto package = json.substr(package_marker, package_end == std::string::npos ? std::string::npos : package_end - package_marker);
    LauncherUpdate update;
    const auto version = json_string(json, "version");
    const auto url = json_string(package, "url");
    const auto hash = json_string(package, "sha256");
    if (!version || !url || !hash) { error = "更新清单字段不完整。"; return std::nullopt; }
    update.version = *version;
    update.url = *url;
    update.sha256 = *hash;
    if (const auto alternate = json_string(package, "fallbackUrl")) update.fallback_url = *alternate;
    return update;
}

bool stage_launcher_update(const LauncherUpdate& update, std::string& error) {
    if (!std::regex_match(update.sha256, std::regex("[0-9a-fA-F]{64}"))) {
        error = "更新清单的 SHA-256 无效，已拒绝下载。";
        return false;
    }
    std::vector<unsigned char> package;
    if (use_official_source.load()) {
        if (update.fallback_url.empty() || !download(update.fallback_url, package, error)) return false;
    } else if (!download(update.url, package, error) &&
               (update.fallback_url.empty() || !download(update.fallback_url, package, error))) {
        return false;
    }
    auto expected = update.sha256;
    std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (sha256(package) != expected) { error = "启动器更新包 SHA-256 校验失败。"; return false; }

    const auto update_root = state_directory() / "updates" / update.version;
    const auto extract_root = update_root / "package";
    std::error_code filesystem_error;
    std::filesystem::remove_all(update_root, filesystem_error);
    std::filesystem::create_directories(extract_root, filesystem_error);
    const auto archive = update_root / "launcher.zip";
    if (!write_binary(archive, package)) { error = "无法保存启动器更新包。"; return false; }
    const auto extracted = capture("tar.exe -xf \"" + path_utf8(archive) + "\" -C \"" + path_utf8(extract_root) + "\"");
    if (extracted.exit_code != 0) { error = "无法解压启动器更新包。"; return false; }
    std::filesystem::path new_launcher;
    for (const auto& item : std::filesystem::recursive_directory_iterator(extract_root, filesystem_error)) {
        if (item.is_regular_file() && _wcsicmp(item.path().filename().c_str(), L"dsh-launcher.exe") == 0) {
            new_launcher = item.path();
            break;
        }
    }
    if (new_launcher.empty()) { error = "更新包内没有 dsh-launcher.exe。"; return false; }
    const auto staged = update_root / "dsh-launcher.new.exe";
    std::filesystem::copy_file(new_launcher, staged, std::filesystem::copy_options::overwrite_existing, filesystem_error);
    if (filesystem_error) { error = "无法暂存新版启动器。"; return false; }

    const auto target = current_executable();
    const auto updater = target.parent_path() / "dsh-updater.exe";
    if (!std::filesystem::exists(updater)) { error = "发布包缺少 dsh-updater.exe。"; return false; }
    const auto backup = target.wstring() + L".old";
    std::wstring command = L"\"" + updater.wstring() + L"\" --pid " + std::to_wstring(GetCurrentProcessId()) +
                           L" --source \"" + staged.wstring() + L"\" --target \"" + target.wstring() +
                           L"\" --backup \"" + backup + L"\" --restart";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, target.parent_path().c_str(), &startup, &process)) {
        error = "无法启动安全更新助手。";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool install_managed_dsh(std::string& error) {
    const auto prefix = runtime_directory() / "dsh";
    std::error_code filesystem_error;
    std::filesystem::create_directories(prefix, filesystem_error);
    const auto npm = executable_command(managed_node_directory() / "npm.cmd", "npm.cmd");
    const auto node_path = path_utf8(managed_node_directory());
    const auto prefix_path = path_utf8(prefix);
    const std::string options = " install --prefix \"" + prefix_path + "\" @deepseek-ai/dsh --no-fund --no-audit";
    const bool official = use_official_source.load();
    auto command = "set \"PATH=" + node_path + ";%PATH%\" && " + npm + options +
                   (official ? " --registry=https://registry.npmjs.org" : " --registry=https://registry.npmmirror.com") +
                   " --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs";
    auto result = capture(command, 180000);
    if (result.exit_code != 0 && !official) {
        command = "set \"PATH=" + node_path + ";%PATH%\" && " + npm + options +
                  " --registry=https://registry.npmjs.org --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs";
        result = capture(command, 180000);
    }
    if (result.exit_code != 0 || !std::filesystem::exists(managed_dsh_command())) {
        error = result.output.empty() ? "DSH 下载失败，请检查网络后重试。" : result.output;
        return false;
    }
    remember_dsh(managed_dsh_command());
    return true;
}

bool update_dsh_at(const std::string& executable, std::string& error) {
    const auto prefix = std::filesystem::path(utf8_to_wide(executable)).parent_path();
    if (prefix.empty() || !std::filesystem::exists(prefix)) {
        error = "无法识别 DSH 原安装目录。";
        return false;
    }
    std::error_code filesystem_error;
    const auto npm = executable_command(managed_node_directory() / "npm.cmd", "npm.cmd");
    const auto node_path = path_utf8(managed_node_directory());
    const auto prefix_path = path_utf8(prefix);
    const std::string options = " install --prefix \"" + prefix_path + "\" @deepseek-ai/dsh --no-fund --no-audit";
    const bool official = use_official_source.load();
    auto command = "set \"PATH=" + node_path + ";%PATH%\" && " + npm + options +
                   (official ? " --registry=https://registry.npmjs.org" : " --registry=https://registry.npmmirror.com") +
                   " --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs";
    auto result = capture(command, 180000);
    if (result.exit_code != 0 && !official) {
        command = "set \"PATH=" + node_path + ";%PATH%\" && " + npm + options +
                  " --registry=https://registry.npmjs.org --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs";
        result = capture(command, 180000);
    }
    if (result.exit_code != 0 || !std::filesystem::exists(std::filesystem::path(utf8_to_wide(executable)))) {
        error = result.output.empty() ? "DSH 下载失败，请检查网络后重试。" : result.output;
        return false;
    }
    remember_dsh(std::filesystem::path(utf8_to_wide(executable)));
    return true;
}

std::string dsh_version(const std::string& executable) {
    const auto node_path = path_utf8(managed_node_directory());
    return first_line(capture("set \"PATH=" + node_path + ";%PATH%\" && \"" + executable + "\" --version 2>nul").output);
}

std::optional<std::string> latest_dsh_version() {
    const auto npm = executable_command(managed_node_directory() / "npm.cmd", "npm.cmd");
    if (use_official_source.load()) {
        return extract_version(capture(npm + " view @deepseek-ai/dsh version --registry=https://registry.npmjs.org 2>nul").output);
    }
    auto result = capture(npm + " view @deepseek-ai/dsh version --registry=https://registry.npmmirror.com 2>nul");
    auto version = extract_version(result.output);
    if (version) return version;
    result = capture(npm + " view @deepseek-ai/dsh version --registry=https://registry.npmjs.org 2>nul");
    return extract_version(result.output);
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
    const bool connected = connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
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
    const auto node_path = managed_node_directory().wstring();
    std::wstring command = L"cmd.exe /d /s /c \"set \"PATH=" + node_path + L";%PATH%\" && \"" + wide_executable + L"\" web >> " + quote(service_log) + L" 2>&1\"";
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
    const auto result = capture("taskkill.exe /PID " + std::to_string(pid) + " /T /F");
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

#ifdef _WIN32

#include "platform/platform.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
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
};

CommandResult capture(const std::string& command) {
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

    CommandResult result;
    std::array<char, 512> buffer{};
    DWORD bytes_read{};
    while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) && bytes_read > 0) {
        if (result.output.size() < 4096) result.output.append(buffer.data(), bytes_read);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &result.exit_code);
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

std::string powershell_encoded_command(const std::string& script) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto wide = utf8_to_wide(script);
    const auto* bytes = reinterpret_cast<const unsigned char*>(wide.data());
    const std::size_t byte_count = wide.size() * sizeof(wchar_t);
    std::string encoded;
    encoded.reserve(((byte_count + 2) / 3) * 4);
    for (std::size_t index = 0; index < byte_count; index += 3) {
        const unsigned value = (static_cast<unsigned>(bytes[index]) << 16) |
                               (index + 1 < byte_count ? static_cast<unsigned>(bytes[index + 1]) << 8 : 0) |
                               (index + 2 < byte_count ? static_cast<unsigned>(bytes[index + 2]) : 0);
        encoded.push_back(alphabet[(value >> 18) & 63]);
        encoded.push_back(alphabet[(value >> 12) & 63]);
        encoded.push_back(index + 1 < byte_count ? alphabet[(value >> 6) & 63] : '=');
        encoded.push_back(index + 2 < byte_count ? alphabet[value & 63] : '=');
    }
    return "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + encoded;
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
    const auto managed = managed_dsh_command();
    if (std::filesystem::exists(managed)) return path_utf8(managed);
    const auto result = capture("where.exe dsh 2>nul");
    const auto path = first_line(result.output);
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

    const std::string script =
        "$ErrorActionPreference='Stop';"
        "$index=Invoke-RestMethod 'https://nodejs.org/dist/index.json';"
        "$release=$index|Where-Object {$_.lts -and $_.files -contains 'win-x64-zip'}|Select-Object -First 1;"
        "if(-not $release){throw 'No compatible Node.js LTS release found'};"
        "$version=$release.version;"
        "$zip=Join-Path $env:TEMP ('dsh-node-'+$version+'.zip');"
        "$url='https://nodejs.org/dist/'+$version+'/node-'+$version+'-win-x64.zip';"
        "Invoke-WebRequest -UseBasicParsing $url -OutFile $zip;"
        "Expand-Archive -LiteralPath $zip -DestinationPath '" + path_utf8(staging) + "' -Force;"
        "$root=Get-ChildItem -LiteralPath '" + path_utf8(staging) + "' -Directory|Select-Object -First 1;"
        "New-Item -ItemType Directory -Force -Path '" + path_utf8(target) + "'|Out-Null;"
        "Copy-Item -Path ($root.FullName+'\\*') -Destination '" + path_utf8(target) + "' -Recurse -Force;"
        "Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue";
    const auto result = capture(powershell_encoded_command(script));
    std::filesystem::remove_all(staging, filesystem_error);
    if (result.exit_code != 0 || !std::filesystem::exists(target / "node.exe") || !std::filesystem::exists(target / "npm.cmd")) {
        error = result.output.empty() ? "Node.js 下载或解压失败，请检查网络后重试。" : result.output;
        return false;
    }
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
    auto command = "set \"PATH=" + node_path + ";%PATH%\" && " + npm + options +
                   " --registry=https://registry.npmmirror.com --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs";
    auto result = capture(command);
    if (result.exit_code != 0) {
        command = "set \"PATH=" + node_path + ";%PATH%\" && " + npm + options +
                  " --registry=https://registry.npmjs.org --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs";
        result = capture(command);
    }
    if (result.exit_code != 0 || !std::filesystem::exists(managed_dsh_command())) {
        error = result.output.empty() ? "DSH 下载失败，请检查网络后重试。" : result.output;
        return false;
    }
    return true;
}

std::string dsh_version(const std::string& executable) {
    const auto node_path = path_utf8(managed_node_directory());
    return first_line(capture("set \"PATH=" + node_path + ";%PATH%\" && \"" + executable + "\" --version 2>nul").output);
}

std::optional<std::string> latest_dsh_version() {
    const auto npm = executable_command(managed_node_directory() / "npm.cmd", "npm.cmd");
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

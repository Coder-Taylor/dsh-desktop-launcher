#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

namespace {

std::wstring argument(int count, wchar_t** values, const wchar_t* name) {
    for (int index = 1; index + 1 < count; ++index) {
        if (_wcsicmp(values[index], name) == 0) return values[index + 1];
    }
    return {};
}

bool has_flag(int count, wchar_t** values, const wchar_t* name) {
    for (int index = 1; index < count; ++index) if (_wcsicmp(values[index], name) == 0) return true;
    return false;
}

bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& target,
                  const std::filesystem::path& backup) {
    std::error_code error;
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::rename(target, backup, error);
    if (error) return false;
    error.clear();
    std::filesystem::rename(source, target, error);
    if (!error) return true;
    std::error_code restore_error;
    std::filesystem::rename(backup, target, restore_error);
    return false;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int count{};
    auto** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return 2;
    const auto pid_text = argument(count, values, L"--pid");
    const auto source = argument(count, values, L"--source");
    const auto target = argument(count, values, L"--target");
    const auto backup = argument(count, values, L"--backup");
    const bool restart = has_flag(count, values, L"--restart");
    LocalFree(values);
    if (pid_text.empty() || source.empty() || target.empty() || backup.empty()) return 2;

    const auto pid = static_cast<DWORD>(_wcstoui64(pid_text.c_str(), nullptr, 10));
    if (const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (replace_file(source, target, backup)) {
            if (restart) {
                const auto target_path = std::filesystem::path(target);
                ShellExecuteW(nullptr, L"open", target_path.c_str(), nullptr, target_path.parent_path().c_str(), SW_SHOWNORMAL);
            }
            return 0;
        }
        Sleep(250);
    }
    return 1;
}

#endif

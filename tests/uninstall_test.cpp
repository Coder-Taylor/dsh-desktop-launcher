#ifdef _WIN32

#include "platform/platform.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <windows.h>

namespace {

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "test\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--cleanup-directory") {
        return dsh::platform::cleanup_launcher_artifact(argv[2]) ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--existing") {
        std::string error;
        if (!dsh::platform::uninstall_dsh(error)) {
            std::cerr << "existing-install uninstall failed: " << error << '\n';
            return 1;
        }
        std::cout << "existing-install uninstall: PASS\n";
        return 0;
    }
    const auto suffix = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
                      (L"dsh-launcher-uninstall-test-" + suffix);
    const auto state = root / L"state";
    const auto prefix = root / L"custom-dsh";
    const auto executable = prefix / L"node_modules" / L".bin" / L"dsh.cmd";

    _wputenv_s(L"LOCALAPPDATA", state.wstring().c_str());
    touch(executable);
    touch(prefix / L"node_modules" / L"@deepseek-ai" / L"peer" / L"index.js");
    touch(prefix / L"package.json");
    touch(prefix / L"package-lock.json");
    touch(prefix / L".dsh-launcher-retired-rollback-test" / L"node_modules" / L"old.js");
    touch(prefix / L"keep-user-file.txt");
    if (dsh::platform::cleanup_launcher_artifact(root) ||
        !std::filesystem::exists(prefix / L"keep-user-file.txt")) {
        std::cerr << "cleanup safety guard accepted a non-launcher directory\n";
        return 1;
    }

    const auto state_directory = state / L"DshLauncher";
    std::filesystem::create_directories(state_directory);
    std::ofstream(state_directory / L"dsh-location.txt") << utf8(executable.wstring()) << '\n';

    std::string error;
    if (!dsh::platform::uninstall_dsh(error)) {
        std::cerr << "uninstall failed: " << error << '\n';
        return 1;
    }
    const bool node_modules_removed = !std::filesystem::exists(prefix / L"node_modules");
    const bool package_removed = !std::filesystem::exists(prefix / L"package.json");
    const bool lock_removed = !std::filesystem::exists(prefix / L"package-lock.json");
    const bool user_file_preserved = std::filesystem::exists(prefix / L"keep-user-file.txt");
    const bool record_removed = !std::filesystem::exists(state_directory / L"dsh-location.txt");
    const bool valid = node_modules_removed && package_removed && lock_removed &&
                       user_file_preserved && record_removed;
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (!valid) {
        std::cerr << "uninstall did not remove exactly the launcher-owned artifacts: "
                  << "node_modules_removed=" << node_modules_removed
                  << ", package_removed=" << package_removed
                  << ", lock_removed=" << lock_removed
                  << ", user_file_preserved=" << user_file_preserved
                  << ", record_removed=" << record_removed << '\n';
        return 1;
    }
    std::cout << "uninstall_test: PASS\n";
    return 0;
}

#endif

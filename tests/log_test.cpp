#include "core/log.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
                      ("dsh-launcher-log-test-" + suffix);

    {
        dsh::Log log(root / "state");
        log.info("primary log test");
        assert(log.used_path() == log.path());
        assert(std::filesystem::exists(log.used_path()));
    }

    const auto blocking_file = root / "blocked-state";
    std::filesystem::create_directories(root);
    std::ofstream(blocking_file) << "not a directory";
    std::filesystem::path fallback;
    {
        dsh::Log log(blocking_file);
        log.info("fallback log test");
        fallback = log.fallback_path();
        assert(log.used_path() == fallback);
        assert(std::filesystem::exists(fallback));
    }

    std::error_code error;
    std::filesystem::remove(fallback, error);
    std::filesystem::remove_all(root, error);
    return 0;
}

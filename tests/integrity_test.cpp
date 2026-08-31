#ifdef _WIN32

#include "platform/platform.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    assert(stream.good());
}

}  // namespace

int main() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto prefix = std::filesystem::temp_directory_path() /
                        std::filesystem::path(L"DSH 完整性测试") /
                        std::filesystem::path(suffix);
    const auto shim = prefix / "node_modules" / ".bin" / "dsh.cmd";
    const auto package = prefix / "node_modules" / "@deepseek-ai" / "dsh" / "package.json";

    write_text(shim, "@echo off\r\necho 0.1.1-rc.2\r\n");
    write_text(package, "{\"name\":\"@deepseek-ai/dsh\",\"version\":\"0.1.1-rc.2\"}\n");

    auto result = dsh::platform::inspect_dsh_installation_at(prefix, shim, true);
    assert(result.complete);
    assert(result.version == "0.1.1-rc.2");

    std::filesystem::remove(shim);
    result = dsh::platform::inspect_dsh_installation_at(prefix, shim, true);
    assert(!result.complete && !result.shim_present);
    write_text(shim, "@echo off\r\necho 0.1.1-rc.2\r\n");

    write_text(package, "{\"name\":\"@deepseek-ai/dsh\",\"version\":\"0.1.1-rc.1\"}\n");
    result = dsh::platform::inspect_dsh_installation_at(prefix, shim, true);
    assert(!result.complete && !result.version_matches_package);
    write_text(package, "{\"name\":\"@deepseek-ai/dsh\",\"version\":\"0.1.1-rc.2\"}\n");

    std::filesystem::create_directories(prefix / ".dsh-launcher-staging");
    result = dsh::platform::inspect_dsh_installation_at(prefix, shim, true);
    assert(!result.complete && result.transaction_pending);
    std::filesystem::remove_all(prefix / ".dsh-launcher-staging");

    result = dsh::platform::inspect_dsh_installation_at(prefix, shim, false);
    assert(!result.complete && !result.node_available);

    std::error_code error;
    std::filesystem::remove_all(prefix.parent_path(), error);
    return 0;
}

#endif

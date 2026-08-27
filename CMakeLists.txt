cmake_minimum_required(VERSION 3.24)

project(dsh_desktop_launcher VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dsh_core
    src/core/log.cpp
    src/core/semver.cpp
    src/core/service.cpp
)
target_include_directories(dsh_core PUBLIC src)

if(WIN32)
    target_sources(dsh_core PRIVATE src/platform/platform_windows.cpp)
    target_link_libraries(dsh_core PRIVATE ws2_32 shell32 winhttp ole32 uuid)
    add_executable(dsh-launcher WIN32 src/app/main_windows.cpp resources/windows/app.rc)
    target_link_libraries(dsh-launcher PRIVATE dsh_core comctl32 dwmapi)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    target_sources(dsh_core PRIVATE src/platform/platform_linux.cpp)
    add_executable(dsh-launcher src/app/main_linux.cpp)
    target_link_libraries(dsh-launcher PRIVATE dsh_core PkgConfig::GTK3)
endif()

if(MINGW)
    target_link_options(dsh-launcher PRIVATE -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_executable(dsh-core-tests
    tests/semver_test.cpp
)
target_link_libraries(dsh-core-tests PRIVATE dsh_core)
add_test(NAME dsh-core-tests COMMAND dsh-core-tests)

install(TARGETS dsh-launcher RUNTIME DESTINATION .)

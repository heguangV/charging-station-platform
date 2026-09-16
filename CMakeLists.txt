cmake_minimum_required(VERSION 3.22)

project(NcsChargingStation VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
endif()

set(PROJECT_MINIMUM_QT_VERSION 6.2)
option(NCS_BUILD_ANDROID_EXPERIMENT "Build the Qt Quick Android user client" OFF)
option(NCS_BUILD_LEGACY_DEMO "Build the original Qt learning demo" OFF)
option(NCS_ENABLE_STRICT_WARNINGS "Treat project warnings as errors" ON)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "NCS Windows builds require MSVC 2019/2022 and a matching Qt MSVC kit. "
        "Initialize the Visual Studio developer environment before configuring; "
        "the current compiler is '${CMAKE_CXX_COMPILER_ID}'."
    )
endif()

include(FetchContent)

set(CROW_ENABLE_SSL ON CACHE BOOL "Enable HTTPS support for NCS" FORCE)
set(CROW_ENABLE_COMPRESSION OFF CACHE BOOL "" FORCE)
find_package(Git REQUIRED)
FetchContent_Declare(asio_source
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(asio_source)
if(NOT asio_source_POPULATED)
    FetchContent_Populate(asio_source)
endif()
set(ASIO_INCLUDE_DIR "${asio_source_SOURCE_DIR}/asio/include" CACHE PATH
    "Standalone Asio include directory" FORCE)

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CROW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.3.3
    GIT_SHALLOW TRUE
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/crow-http-body-limit.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_crow_patch.cmake"
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/crow-websocket-payload-1009.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_crow_patch.cmake"
)
FetchContent_MakeAvailable(Crow)

find_package(OpenSSL REQUIRED)
if(BUILD_TESTING)
    # SQLite remains test-only while the historical contract suite is ported to PostgreSQL.
    # Production builds and ncs_server have no SQLite link or runtime dependency.
    find_package(SQLite3 REQUIRED)
endif()

add_library(ncs_crow INTERFACE)
add_library(ncs::crow ALIAS ncs_crow)
target_compile_definitions(ncs_crow INTERFACE
    CROW_ENABLE_SSL
    CROW_HTTP_MAX_BODY_SIZE=8388608
)
target_link_libraries(ncs_crow INTERFACE
    Crow::Crow
    OpenSSL::SSL
    OpenSSL::Crypto
)

find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} REQUIRED COMPONENTS
    Core
    Widgets
    Network
    Sql
)

# Qt Charts 曾供原 Qt 管理端图表使用；管理端改为 Web 后不再需要。
# WebEngineWidgets 曾在记录中被提及，但正式目标从未依赖它。


set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

function(ncs_set_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
        if(NCS_ENABLE_STRICT_WARNINGS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(NCS_ENABLE_STRICT_WARNINGS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_subdirectory(core)
add_subdirectory(infrastructure)
# Agent 一级模块：依赖方向 server → agent → core / infrastructure。
add_subdirectory(agent)
# apps/user（车主端）与 apps/admin（管理端）均已改造为 apps/user、apps/admin 下的
# Vue 3 + Vite 响应式 Web，用 npm 独立构建，不进入 CMake C++ 流程，因此这里不再有客户端 C++ 目标；
# apps/mobile 实验默认不构建，保留为迁移参考。C++ 正式可执行目标只剩 ncs_server。
add_subdirectory(server)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(NCS_BUILD_LEGACY_DEMO)
    qt_add_executable(codex-qt-demo
        src/main.cpp
        src/mainwindow.cpp
        src/mainwindow.h
    )
    target_link_libraries(codex-qt-demo PRIVATE Qt6::Widgets)
    ncs_set_project_warnings(codex-qt-demo)
endif()

if(NCS_BUILD_ANDROID_EXPERIMENT)
    find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Quick)
    add_subdirectory(apps/mobile)
endif()

# 客户端改为 Web 后，正式 C++ 可执行目标只剩服务端；Web 产物由 npm run build 生成到各应用 dist/。
install(TARGETS ncs_server RUNTIME DESTINATION bin)

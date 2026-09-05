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
option(NCS_BUILD_ANDROID_EXPERIMENT "Build the future QML/Android experiment" OFF)
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
find_package(SQLite3 REQUIRED)

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

find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} QUIET COMPONENTS Charts)
find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} QUIET COMPONENTS WebEngineWidgets)

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
add_subdirectory(apps/user)
add_subdirectory(apps/admin)
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
    find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Quick WebView)
    add_subdirectory(apps/mobile)
endif()

install(TARGETS ncs_user ncs_admin ncs_server RUNTIME DESTINATION bin)

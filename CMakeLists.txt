cmake_minimum_required(VERSION 3.24)

project(NcsChargingStation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} REQUIRED COMPONENTS
    Core
    Widgets
    Network
    Sql
)

find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} QUIET COMPONENTS
    Charts
    WebEngineWidgets
)

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

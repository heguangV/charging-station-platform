cmake_minimum_required(VERSION 3.16)

project(NcsChargingStation VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROJECT_MINIMUM_QT_VERSION 6.5.3)
option(NCS_BUILD_ANDROID_EXPERIMENT "Build the future QML/Android experiment" OFF)

find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} REQUIRED COMPONENTS
    Widgets
    Network
    Sql
)

find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} QUIET COMPONENTS
    Charts
    WebEngineWidgets
)

qt_standard_project_setup(REQUIRES 6.5)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(codex-qt-demo
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
)

target_link_libraries(codex-qt-demo PRIVATE Qt6::Widgets)

if(NCS_BUILD_ANDROID_EXPERIMENT)
    find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Quick WebView)
    add_subdirectory(apps/mobile)
endif()

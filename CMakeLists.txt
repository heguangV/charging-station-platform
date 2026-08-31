cmake_minimum_required(VERSION 3.16)

project(CodexQtDemo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROJECT_MINIMUM_QT_VERSION 6.8)
find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(codex-qt-demo
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
)

target_link_libraries(codex-qt-demo PRIVATE Qt6::Widgets)

cmake_minimum_required(VERSION 3.22)
project(NcsMobile LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Multimedia Positioning WebView)

qt_add_executable(ncs_mobile main.cpp mobile_api.cpp mobile_profile.cpp mobile_review.cpp mobile_location.cpp mobile_api.h)
qt_add_qml_module(ncs_mobile
    URI NcsMobile
    VERSION 1.0
    RESOURCE_PREFIX "/qt/qml"
    QML_FILES Main.qml StationDetail.qml
)
target_link_libraries(ncs_mobile PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Multimedia Qt6::Positioning Qt6::WebView)
target_compile_definitions(ncs_mobile PRIVATE NCS_HAS_POSITIONING)
set_target_properties(ncs_mobile PROPERTIES
    QT_ANDROID_PACKAGE_NAME "com.ncs.charging"
    QT_ANDROID_APP_NAME "NCS 充电"
    QT_ANDROID_VERSION_NAME "1.0"
    QT_ANDROID_VERSION_CODE "1"
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
)

cmake_minimum_required(VERSION 3.24)
project(NcsMobileContractTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Test)
enable_testing()
add_executable(ncs_mobile_contract_tests mobile_contract_test.cpp
    ../../apps/mobile/mobile_api.cpp ../../apps/mobile/mobile_profile.cpp
    ../../apps/mobile/mobile_review.cpp ../../apps/mobile/mobile_location.cpp ../../apps/mobile/mobile_api.h)
target_include_directories(ncs_mobile_contract_tests PRIVATE ../../apps/mobile)
target_link_libraries(ncs_mobile_contract_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
add_test(NAME ncs_mobile_contract COMMAND ncs_mobile_contract_tests)
set_tests_properties(ncs_mobile_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "mobile;contract;network")

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Quick Qml QuickControls2)
file(GLOB NCS_USER_LAYOUT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/user/user_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/user/ui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/user/ui/*.h")
add_executable(ncs_station_layout_tests station_layout_test.cpp ${NCS_USER_LAYOUT_SOURCES}
    ../../apps/user/net/api_client.cpp ../../apps/user/net/api_client.h ../../apps/user/net/user_api.cpp)
target_include_directories(ncs_station_layout_tests PRIVATE ../../apps/user)
target_link_libraries(ncs_station_layout_tests PRIVATE Qt6::Widgets Qt6::Network Qt6::Test)
qt_add_resources(ncs_station_layout_tests layout_assets PREFIX "/ncs" BASE "../../apps/user"
    FILES ../../apps/user/resources/charging-scene.png ../../apps/user/resources/navigation-chevron.svg)
add_test(NAME ncs_station_layout COMMAND ncs_station_layout_tests)
set_tests_properties(ncs_station_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "user;ui;contract")

add_executable(ncs_mobile_station_layout_tests mobile_station_layout_test.cpp)
target_link_libraries(ncs_mobile_station_layout_tests PRIVATE Qt6::Quick Qt6::Qml Qt6::QuickControls2 Qt6::Test)
target_compile_definitions(ncs_mobile_station_layout_tests PRIVATE
    NCS_STATION_QML="${CMAKE_CURRENT_SOURCE_DIR}/../../apps/mobile/StationDetail.qml")
add_test(NAME ncs_mobile_station_layout COMMAND ncs_mobile_station_layout_tests)
set_tests_properties(ncs_mobile_station_layout PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software" LABELS "mobile;ui;contract")

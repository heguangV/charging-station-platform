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

find_package(Qt6 6.8 REQUIRED COMPONENTS Quick Qml QuickControls2)

add_executable(ncs_mobile_station_layout_tests mobile_station_layout_test.cpp)
target_link_libraries(ncs_mobile_station_layout_tests PRIVATE Qt6::Quick Qt6::Qml Qt6::QuickControls2 Qt6::Test)
target_compile_definitions(ncs_mobile_station_layout_tests PRIVATE
    NCS_STATION_QML="${CMAKE_CURRENT_SOURCE_DIR}/../../apps/mobile/StationDetail.qml")
add_test(NAME ncs_mobile_station_layout COMMAND ncs_mobile_station_layout_tests)
set_tests_properties(ncs_mobile_station_layout PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software" LABELS "mobile;ui;contract")

cmake_minimum_required(VERSION 3.24)
project(ncs_dashboard_checks NONE)
enable_testing()
find_program(PNPM_EXECUTABLE pnpm REQUIRED)
add_test(NAME dashboard_contract COMMAND ${PNPM_EXECUTABLE} test)
set_tests_properties(dashboard_contract PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LABELS "unit;contract;dashboard")
add_test(NAME dashboard_ui COMMAND ${PNPM_EXECUTABLE} run test:ui)
set_tests_properties(dashboard_ui PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LABELS "ui;dashboard"
    TIMEOUT 180)

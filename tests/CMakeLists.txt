add_executable(ncs_admin_routes_tests
    admin_routes_test.cpp
)

set_target_properties(ncs_admin_routes_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_admin_routes_tests PRIVATE
    ncs::server_controller
    Qt6::Core
    Qt6::Gui
)
add_test(NAME ncs_admin_routes COMMAND ncs_admin_routes_tests)

add_executable(ncs_server_config_tests
    server_config_test.cpp
)

set_target_properties(ncs_server_config_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_server_config_tests PRIVATE
    ncs::server_runtime
    Qt6::Core
)

add_test(NAME ncs_server_config COMMAND ncs_server_config_tests)

add_executable(ncs_api_routes_tests
    api_routes_test.cpp
)

set_target_properties(ncs_api_routes_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_api_routes_tests PRIVATE ncs::server_controller)

add_test(NAME ncs_api_routes COMMAND ncs_api_routes_tests)

add_executable(ncs_structured_logger_tests
    structured_logger_test.cpp
)

set_target_properties(ncs_structured_logger_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_structured_logger_tests PRIVATE
    ncs::server_middleware
    Qt6::Core
)

add_test(NAME ncs_structured_logger COMMAND ncs_structured_logger_tests)

add_executable(ncs_global_exception_handler_tests
    global_exception_handler_test.cpp
)

set_target_properties(ncs_global_exception_handler_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_global_exception_handler_tests PRIVATE
    ncs::server_middleware
    Qt6::Core
)

add_test(NAME ncs_global_exception_handler COMMAND ncs_global_exception_handler_tests)

add_executable(ncs_security_services_tests
    security_services_test.cpp
)

set_target_properties(ncs_security_services_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_security_services_tests PRIVATE ncs::core_application)
add_test(NAME ncs_security_services COMMAND ncs_security_services_tests)

add_executable(ncs_idempotency_service_tests
    idempotency_service_test.cpp
)

set_target_properties(ncs_idempotency_service_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_idempotency_service_tests PRIVATE ncs::core_application)
add_test(NAME ncs_idempotency_service COMMAND ncs_idempotency_service_tests)

add_executable(ncs_common_http_tests
    common_http_test.cpp
)

set_target_properties(ncs_common_http_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_common_http_tests PRIVATE
    ncs::server_controller
    ncs::server_middleware
    Qt6::Core
)
add_test(NAME ncs_common_http COMMAND ncs_common_http_tests)

add_executable(ncs_user_identity_routes_tests
    user_identity_routes_test.cpp
)

set_target_properties(ncs_user_identity_routes_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

target_link_libraries(ncs_user_identity_routes_tests PRIVATE
    ncs::server_controller
    Qt6::Core
    Qt6::Gui
)
add_test(NAME ncs_user_identity_routes COMMAND ncs_user_identity_routes_tests)

add_executable(ncs_charge_flow_service_tests
    charge_flow_service_test.cpp
)

set_target_properties(ncs_charge_flow_service_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_charge_flow_service_tests PRIVATE ncs::core_application)
add_test(NAME ncs_charge_flow_service COMMAND ncs_charge_flow_service_tests)

add_executable(ncs_user_business_routes_tests
    user_business_routes_test.cpp
)

set_target_properties(ncs_user_business_routes_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_user_business_routes_tests PRIVATE
    ncs::server_controller
    ncs::infrastructure_map
    Qt6::Core
    Qt6::Gui
)
add_test(NAME ncs_user_business_routes COMMAND ncs_user_business_routes_tests)

add_executable(ncs_sqlite_repository_tests
    sqlite_repository_test.cpp
)

set_target_properties(ncs_sqlite_repository_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_sqlite_repository_tests PRIVATE
    ncs::core_application
    ncs::infrastructure_sqlite
)
add_test(NAME ncs_sqlite_repository COMMAND ncs_sqlite_repository_tests)

add_executable(ncs_sqlite_migration_concurrency_tests
    sqlite_migration_concurrency_test.cpp
)

set_target_properties(ncs_sqlite_migration_concurrency_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_sqlite_migration_concurrency_tests PRIVATE
    ncs::core_application
    ncs::infrastructure_sqlite
)
add_test(NAME ncs_sqlite_migration_concurrency COMMAND ncs_sqlite_migration_concurrency_tests)

add_executable(ncs_station_service_tests
    station_service_test.cpp
)

set_target_properties(ncs_station_service_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_station_service_tests PRIVATE ncs::core_application)
add_test(NAME ncs_station_service COMMAND ncs_station_service_tests)

add_executable(ncs_navigation_service_tests
    navigation_service_test.cpp
)
set_target_properties(ncs_navigation_service_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(ncs_navigation_service_tests PRIVATE ncs::core_application)
add_test(NAME ncs_navigation_service COMMAND ncs_navigation_service_tests)

add_executable(ncs_tencent_route_planner_tests
    tencent_route_planner_test.cpp
)
set_target_properties(ncs_tencent_route_planner_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(ncs_tencent_route_planner_tests PRIVATE
    ncs::infrastructure_map
    Qt6::Core
)
add_test(NAME ncs_tencent_route_planner COMMAND ncs_tencent_route_planner_tests)

add_executable(ncs_websocket_hub_tests
    websocket_hub_test.cpp
)

set_target_properties(ncs_websocket_hub_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_websocket_hub_tests PRIVATE ncs::core_application)
add_test(NAME ncs_websocket_hub COMMAND ncs_websocket_hub_tests)

add_executable(ncs_websocket_dispatcher_tests
    websocket_dispatcher_test.cpp
)

set_target_properties(ncs_websocket_dispatcher_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_websocket_dispatcher_tests PRIVATE
    ncs::server_websocket
    Qt6::Core
)
add_test(NAME ncs_websocket_dispatcher COMMAND ncs_websocket_dispatcher_tests)

add_executable(ncs_websocket_routes_tests
    websocket_routes_test.cpp
)

set_target_properties(ncs_websocket_routes_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_websocket_routes_tests PRIVATE
    ncs::server_websocket
    Qt6::Core
)
add_test(NAME ncs_websocket_routes COMMAND ncs_websocket_routes_tests)

add_executable(ncs_periodic_scheduler_tests
    periodic_scheduler_test.cpp
)

set_target_properties(ncs_periodic_scheduler_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_periodic_scheduler_tests PRIVATE ncs::server_runtime)
add_test(NAME ncs_periodic_scheduler COMMAND ncs_periodic_scheduler_tests)

add_executable(ncs_dashboard_ml_routes_tests
    dashboard_ml_routes_test.cpp
)
set_target_properties(ncs_dashboard_ml_routes_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(ncs_dashboard_ml_routes_tests PRIVATE
    ncs::server_controller
    ncs::infrastructure_sqlite
    Qt6::Core
    Qt6::Gui
)
add_test(NAME ncs_dashboard_ml_routes COMMAND ncs_dashboard_ml_routes_tests)

add_executable(ncs_ml_process_manager_tests
    ml_process_manager_test.cpp
)
set_target_properties(ncs_ml_process_manager_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(ncs_ml_process_manager_tests PRIVATE
    ncs::server_runtime
    ncs::infrastructure_sqlite
    Qt6::Core
)
add_test(NAME ncs_ml_process_manager COMMAND ncs_ml_process_manager_tests)

add_test(
    NAME ncs_server_build
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ncs_server --parallel 2
)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME ncs_server_smoke
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/server_smoke_test.py
        $<TARGET_FILE:ncs_server>
)
set_tests_properties(ncs_server_smoke PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 300
)

add_test(
    NAME ncs_server_http_smoke
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/server_http_smoke_test.py
        $<TARGET_FILE:ncs_server>
        $<TARGET_FILE:ncs_user>
)
set_tests_properties(ncs_server_http_smoke PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 60
    LABELS "smoke;integration;http;user"
)

# --- Foundation and user-client tests (integrated from develop) ---
qt_add_executable(ncs_foundation_tests
    foundation_tests.cpp
)

target_link_libraries(ncs_foundation_tests
    PRIVATE
        ncs_infrastructure
        Qt6::Core
)

ncs_set_project_warnings(ncs_foundation_tests)

add_test(NAME ncs_foundation_tests COMMAND ncs_foundation_tests)
set_tests_properties(ncs_foundation_tests PROPERTIES LABELS "unit;foundation")

qt_add_executable(ncs_user_net_tests
    user_net_tests.cpp
    ../apps/user/net/api_client.cpp
    ../apps/user/net/api_client.h
)

target_include_directories(ncs_user_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/user)
target_link_libraries(ncs_user_net_tests PRIVATE Qt6::Core Qt6::Network)
ncs_set_project_warnings(ncs_user_net_tests)
add_test(NAME ncs_user_net_tests COMMAND ncs_user_net_tests)
set_tests_properties(ncs_user_net_tests PROPERTIES LABELS "unit;user;network")

add_test(NAME ncs_user_smoke COMMAND ncs_user --smoke-test)
set_tests_properties(ncs_user_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "smoke;ui;foundation"
)
add_test(NAME ncs_admin_smoke COMMAND ncs_admin --smoke-test)
set_tests_properties(ncs_admin_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "smoke;ui;foundation"
)

if(UNIX)
    find_program(NCS_BASH_EXECUTABLE bash)
    find_program(NCS_CLANG_FORMAT_EXECUTABLE clang-format)
    if(NCS_BASH_EXECUTABLE AND NCS_CLANG_FORMAT_EXECUTABLE)
        add_test(
            NAME ncs_check_script
            COMMAND ${NCS_BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/check_script_test.sh
                ${CMAKE_SOURCE_DIR}/scripts/check.sh
                ${CMAKE_SOURCE_DIR}/.clang-format
        )
        set_tests_properties(ncs_check_script PROPERTIES LABELS "unit;repository")
    endif()
endif()

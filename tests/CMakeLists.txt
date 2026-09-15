add_executable(ncs_order_review_tests order_review_test.cpp)
set_target_properties(ncs_order_review_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_order_review_tests PRIVATE ncs::server_controller ncs::infrastructure_sqlite Qt6::Core)
add_test(NAME ncs_order_review COMMAND ncs_order_review_tests)
set_tests_properties(ncs_order_review PROPERTIES LABELS "contract;integration" TIMEOUT 120)

add_executable(ncs_postgres_statement_tests postgres_statement_test.cpp)
set_target_properties(ncs_postgres_statement_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_postgres_statement_tests PRIVATE ncs::infrastructure_postgres Qt6::Core)
add_test(NAME ncs_postgres_statement COMMAND ncs_postgres_statement_tests)
set_tests_properties(ncs_postgres_statement PROPERTIES LABELS "database;unit" TIMEOUT 30)

add_executable(ncs_postgres_repository_tests postgres_repository_test.cpp)
set_target_properties(ncs_postgres_repository_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_link_libraries(ncs_postgres_repository_tests PRIVATE
    ncs::infrastructure_postgres
    Qt6::Core
)
add_test(NAME ncs_postgres_repository
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_postgres_repository_test.sh
        $<TARGET_FILE:ncs_postgres_repository_tests> ${PROJECT_SOURCE_DIR}
)
set_tests_properties(ncs_postgres_repository PROPERTIES
    LABELS "database;integration"
    TIMEOUT 300
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
)

add_executable(ncs_create_sqlite_v10_fixture create_sqlite_v10_fixture.cpp)
set_target_properties(ncs_create_sqlite_v10_fixture PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_create_sqlite_v10_fixture PRIVATE
    ncs::infrastructure_sqlite SQLite3::SQLite3 Qt6::Core)
ncs_set_project_warnings(ncs_create_sqlite_v10_fixture)
add_test(NAME ncs_sqlite_to_postgres
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sqlite_to_postgres_test.sh
        $<TARGET_FILE:ncs_create_sqlite_v10_fixture>
        $<TARGET_FILE:ncs_sqlite_to_postgres>
        ${PROJECT_SOURCE_DIR}/infrastructure/postgres/migrations)
set_tests_properties(ncs_sqlite_to_postgres PROPERTIES
    LABELS "database;integration;migration"
    TIMEOUT 300
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)

add_executable(ncs_sqlite_avatar_repository_tests sqlite_avatar_repository_test.cpp)
set_target_properties(ncs_sqlite_avatar_repository_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_sqlite_avatar_repository_tests PRIVATE ncs::infrastructure_sqlite Qt6::Core)
add_test(NAME ncs_sqlite_avatar_repository COMMAND ncs_sqlite_avatar_repository_tests)
set_tests_properties(ncs_sqlite_avatar_repository PROPERTIES LABELS "unit;repository" TIMEOUT 60)

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

add_executable(ncs_admin_account_routes_tests
    admin_account_routes_test.cpp
)

set_target_properties(ncs_admin_account_routes_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_admin_account_routes_tests PRIVATE
    ncs::server_controller
    Qt6::Core
    Qt6::Gui
)
add_test(NAME ncs_admin_account_routes COMMAND ncs_admin_account_routes_tests)
set_tests_properties(ncs_admin_account_routes PROPERTIES LABELS "contract")

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

add_executable(ncs_database_security_tests database_security_test.cpp)
set_target_properties(ncs_database_security_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_database_security_tests PRIVATE ncs::server_runtime Qt6::Core)
add_test(NAME ncs_database_security COMMAND ncs_database_security_tests)
set_tests_properties(ncs_database_security PROPERTIES LABELS "unit;security;database")

add_executable(ncs_database_config_tests database_config_test.cpp)
set_target_properties(ncs_database_config_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_database_config_tests PRIVATE ncs::server_runtime Qt6::Core)
add_test(NAME ncs_database_config COMMAND ncs_database_config_tests)
set_tests_properties(ncs_database_config PROPERTIES LABELS "unit;config;database")

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

add_executable(ncs_sqlite_admin_accounts_tests
    sqlite_admin_accounts_test.cpp
)

set_target_properties(ncs_sqlite_admin_accounts_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_sqlite_admin_accounts_tests PRIVATE
    ncs::core_application
    ncs::infrastructure_sqlite
)
add_test(NAME ncs_sqlite_admin_accounts COMMAND ncs_sqlite_admin_accounts_tests)
set_tests_properties(ncs_sqlite_admin_accounts PROPERTIES LABELS "database")

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

add_executable(ncs_sqlite_seed_tests
    sqlite_seed_test.cpp
)

set_target_properties(ncs_sqlite_seed_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_sqlite_seed_tests PRIVATE
    ncs::core_application
    ncs::infrastructure_sqlite
)
add_test(NAME ncs_sqlite_seed COMMAND ncs_sqlite_seed_tests)
set_tests_properties(ncs_sqlite_seed PROPERTIES LABELS "unit;database;seed")

add_executable(ncs_sqlite_seed_conflict_tests
    sqlite_seed_conflict_test.cpp
)

set_target_properties(ncs_sqlite_seed_conflict_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_sqlite_seed_conflict_tests PRIVATE
    ncs::infrastructure_sqlite
)
add_test(NAME ncs_sqlite_seed_conflict COMMAND ncs_sqlite_seed_conflict_tests)
set_tests_properties(ncs_sqlite_seed_conflict PROPERTIES LABELS "unit;database;seed")

add_executable(ncs_sqlite_corruption_tests
    sqlite_corruption_test.cpp
)

set_target_properties(ncs_sqlite_corruption_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_sqlite_corruption_tests PRIVATE
    ncs::core_application
    ncs::infrastructure_sqlite
)
add_test(NAME ncs_sqlite_corruption COMMAND ncs_sqlite_corruption_tests)
set_tests_properties(ncs_sqlite_corruption PROPERTIES LABELS "unit;database")

add_executable(ncs_sqlite_revenue_bench
    sqlite_revenue_bench.cpp
)

set_target_properties(ncs_sqlite_revenue_bench PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ncs_sqlite_revenue_bench PRIVATE
    ncs::core_application
    ncs::infrastructure_sqlite
)
add_test(NAME ncs_sqlite_revenue_bench COMMAND ncs_sqlite_revenue_bench)
set_tests_properties(ncs_sqlite_revenue_bench PROPERTIES LABELS "performance")

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
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_server_smoke_test.sh
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/server_smoke_test.py
        $<TARGET_FILE:ncs_server>
        ${PROJECT_SOURCE_DIR}/infrastructure/postgres/migrations
)
set_tests_properties(ncs_server_smoke PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 300
    SKIP_RETURN_CODE 77
)

# 用户端已迁移为 apps/user 下的 Vue 3 + Vite Web 客户端（npm 独立构建），不再是 CMake 目标，
# 因此原先驱动 ncs_user 二进制与 Qt 客户端冒烟测试的用例一并移除，避免引用不存在的目标。

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

# 管理端已改造为 apps/admin 下的 Vue 3 Web 控制台（npm 独立构建），不再是 CMake 目标：
# 原先驱动 Qt 管理端控件的 ncs_admin_ui_contract 与驱动 ncs_admin 二进制登录的
# ncs_admin_api_smoke 一并移除，避免引用不存在的目标。管理端接口契约仍由
# ncs_admin_routes 与 ncs_admin_account_routes 在服务端侧覆盖。

# —— Agent 模块测试：工具调度、四个工具契约、LLM 基础设施与 Agent Controller 契约 ——
add_executable(ncs_agent_service_tests agent_service_test.cpp)
set_target_properties(ncs_agent_service_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)
target_link_libraries(ncs_agent_service_tests PRIVATE ncs::agent)
ncs_set_project_warnings(ncs_agent_service_tests)
add_test(NAME ncs_agent_service COMMAND ncs_agent_service_tests)
set_tests_properties(ncs_agent_service PROPERTIES LABELS "unit;agent" TIMEOUT 60)

add_executable(ncs_agent_tools_tests agent_tools_test.cpp)
set_target_properties(ncs_agent_tools_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)
target_link_libraries(ncs_agent_tools_tests PRIVATE ncs::agent)
ncs_set_project_warnings(ncs_agent_tools_tests)
add_test(NAME ncs_agent_tools COMMAND ncs_agent_tools_tests)
set_tests_properties(ncs_agent_tools PROPERTIES LABELS "unit;agent" TIMEOUT 60)

add_executable(ncs_agent_controller_tests agent_controller_test.cpp)
set_target_properties(ncs_agent_controller_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)
target_link_libraries(ncs_agent_controller_tests PRIVATE ncs::server_controller)
ncs_set_project_warnings(ncs_agent_controller_tests)
add_test(NAME ncs_agent_controller COMMAND ncs_agent_controller_tests)
set_tests_properties(ncs_agent_controller PROPERTIES LABELS "contract;agent" TIMEOUT 60)

add_executable(ncs_llm_client_tests llm_client_test.cpp)
set_target_properties(ncs_llm_client_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)
target_link_libraries(ncs_llm_client_tests PRIVATE ncs::infrastructure_ai)
ncs_set_project_warnings(ncs_llm_client_tests)
add_test(NAME ncs_llm_client COMMAND ncs_llm_client_tests)
set_tests_properties(ncs_llm_client PROPERTIES LABELS "unit;agent;ai" TIMEOUT 60)

add_executable(ncs_order_payment_tests order_payment_test.cpp)
set_target_properties(ncs_order_payment_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_order_payment_tests PRIVATE ncs::server_controller ncs::infrastructure_sqlite Qt6::Core)
add_test(NAME ncs_order_payment COMMAND ncs_order_payment_tests)
set_tests_properties(ncs_order_payment PROPERTIES LABELS "contract;integration;payment" TIMEOUT 120)

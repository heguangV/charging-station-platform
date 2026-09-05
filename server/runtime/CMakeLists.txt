add_library(ncs_server_runtime STATIC
    ml_process_manager.cpp
    ml_process_manager.h
    periodic_scheduler.cpp
    periodic_scheduler.h
    server_config.cpp
    server_config.h
    server_config_environment.cpp
    server_config_environment.h
    startup_checks.cpp
    startup_checks.h
)
add_library(ncs::server_runtime ALIAS ncs_server_runtime)

set_target_properties(ncs_server_runtime PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_server_runtime PUBLIC cxx_std_17)
target_include_directories(ncs_server_runtime PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_definitions(ncs_server_runtime PRIVATE
    NCS_SERVER_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(ncs_server_runtime
    PUBLIC
        ncs::core_application
        ncs::crow
        ncs::structured_log
    PRIVATE
        Qt6::Core
)

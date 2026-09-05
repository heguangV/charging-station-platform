add_library(ncs_server_middleware STATIC
    authorization.cpp
    authorization.h
    crow_log_handler.cpp
    crow_log_handler.h
    global_exception_handler.cpp
    global_exception_handler.h
    request_policy_middleware.cpp
    request_policy_middleware.h
)
add_library(ncs::server_middleware ALIAS ncs_server_middleware)

set_target_properties(ncs_server_middleware PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_server_middleware PUBLIC cxx_std_17)
target_include_directories(ncs_server_middleware PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_server_middleware
    PUBLIC
        ncs::core_application
        ncs::crow
        ncs::http_common
        ncs::structured_log
    PRIVATE
        Qt6::Core
)

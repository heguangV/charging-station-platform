add_library(ncs_server_controller STATIC
    admin_routes.cpp
    admin_routes.h
    api_routes.cpp
    api_routes.h
    dashboard_routes.cpp
    dashboard_routes.h
    async_response.cpp
    async_response.h
    flow_routes.cpp
    flow_routes.h
    health_routes.cpp
    health_routes.h
    ml_routes.cpp
    ml_routes.h
    navigation_routes.cpp
    navigation_routes.h
    idempotent_response.cpp
    idempotent_response.h
    request_validation.cpp
    request_validation.h
    station_routes.cpp
    station_routes.h
    user_auth.h
    user_identity_dto.cpp
    user_identity_dto.h
    user_identity_routes.cpp
    user_identity_routes.h
    wallet_routes.cpp
    wallet_routes.h
)
add_library(ncs::server_controller ALIAS ncs_server_controller)

set_target_properties(ncs_server_controller PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_server_controller PUBLIC cxx_std_17)
target_include_directories(ncs_server_controller PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_server_controller
    PUBLIC
        ncs::core_application
        ncs::crow
        ncs::http_common
        ncs::server_middleware
        ncs::structured_log
        Qt6::Core
        Qt6::Gui
)

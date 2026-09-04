add_library(ncs_core_application STATIC
    admin_auth_service.cpp
    admin_auth_service.h
    admin_repository.cpp
    admin_repository.h
    admin_user_service.cpp
    admin_user_service.h
    analytics_service.cpp
    analytics_service.h
    bounded_executor.cpp
    bounded_executor.h
    admin_ops_service.cpp
    admin_ops_service.h
    admin_station_service.cpp
    admin_station_service.h
    business_numbers.cpp
    business_numbers.h
    charge_flow_service.cpp
    charge_flow_service.h
    charging_repository.cpp
    charging_repository.h
    event_hub.cpp
    event_hub.h
    idempotency_service.cpp
    idempotency_service.h
    in_memory_admin_repository.cpp
    in_memory_admin_repository.h
    in_memory_user_account_repository.cpp
    in_memory_user_account_repository.h
    pricing.cpp
    pricing.h
    readiness_probe.h
    security_crypto.cpp
    security_crypto.h
    session_manager.cpp
    session_manager.h
    station_service.cpp
    station_service.h
    user_account_repository.h
    user_identity_service.cpp
    user_identity_service.h
    verification_code_service.cpp
    verification_code_service.h
    wallet_service.cpp
    wallet_service.h
)
add_library(ncs::core_application ALIAS ncs_core_application)

set_target_properties(ncs_core_application PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_core_application PUBLIC cxx_std_17)
target_include_directories(ncs_core_application PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_core_application
    PUBLIC ncs::core_domain
    PRIVATE OpenSSL::Crypto
)

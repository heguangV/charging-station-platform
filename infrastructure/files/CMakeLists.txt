add_library(ncs_structured_log STATIC
    atomic_snapshot_writer.cpp
    atomic_snapshot_writer.h
    model_artifact_store.cpp
    model_artifact_store.h
    structured_logger.cpp
    structured_logger.h
)
add_library(ncs::structured_log ALIAS ncs_structured_log)

set_target_properties(ncs_structured_log PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_structured_log PUBLIC cxx_std_17)
target_include_directories(ncs_structured_log PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_structured_log
    PUBLIC ncs::core_application
    PRIVATE Qt6::Core OpenSSL::Crypto
)

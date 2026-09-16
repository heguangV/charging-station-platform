add_library(ncs_infrastructure_ai STATIC
    llm_client.cpp
    llm_client.h
    llm_config.cpp
    llm_config.h
)
add_library(ncs::infrastructure_ai ALIAS ncs_infrastructure_ai)

set_target_properties(ncs_infrastructure_ai PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_infrastructure_ai PUBLIC cxx_std_17)
target_include_directories(ncs_infrastructure_ai PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_infrastructure_ai
    PUBLIC
        Qt6::Core
        Qt6::Network
        ncs_core_application
)

ncs_set_project_warnings(ncs_infrastructure_ai)

add_library(ncs_infrastructure_map STATIC
    tencent_geocoder.cpp
    tencent_geocoder.h
)
add_library(ncs::infrastructure_map ALIAS ncs_infrastructure_map)

set_target_properties(ncs_infrastructure_map PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_infrastructure_map PUBLIC cxx_std_17)
target_include_directories(ncs_infrastructure_map PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_infrastructure_map
    PUBLIC
        Qt6::Core
        Qt6::Network
        ncs_core_application
)

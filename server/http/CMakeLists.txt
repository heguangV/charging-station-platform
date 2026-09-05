add_library(ncs_http_common STATIC
    ../controller/api_response.cpp
    ../controller/api_response.h
)
add_library(ncs::http_common ALIAS ncs_http_common)

set_target_properties(ncs_http_common PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_http_common PUBLIC cxx_std_17)
target_include_directories(ncs_http_common PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_http_common
    PUBLIC
        ncs::core_domain
        ncs::crow
        ncs::structured_log
        Qt6::Core
)

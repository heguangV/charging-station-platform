add_library(ncs_infrastructure STATIC
    config/application_config.cpp
    config/application_config.h
    logging/application_logger.cpp
    logging/application_logger.h
)

target_include_directories(ncs_infrastructure
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ncs_infrastructure
    PUBLIC
        ncs_core
        Qt6::Core
)

ncs_set_project_warnings(ncs_infrastructure)

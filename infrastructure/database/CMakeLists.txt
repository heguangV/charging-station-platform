add_library(ncs_infrastructure_database STATIC
    platform_repository.h
    repository_factory.cpp
    repository_factory.h
)
add_library(ncs::infrastructure_database ALIAS ncs_infrastructure_database)

set_target_properties(ncs_infrastructure_database PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_infrastructure_database PUBLIC cxx_std_17)
target_include_directories(ncs_infrastructure_database PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_infrastructure_database
    PUBLIC
        ncs::infrastructure_postgres
)

ncs_set_project_warnings(ncs_infrastructure_database)

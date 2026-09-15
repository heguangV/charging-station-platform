add_library(ncs_infrastructure_postgres STATIC
    postgres_repository.h
    postgres_repository_detail.h
    postgres_row_mappers.h
    postgres_backup_detail.h
    postgres_repository_base.cpp
    postgres_repository_user.cpp
    postgres_repository_flow.cpp
    postgres_repository_order.cpp
    postgres_repository_idempotency.cpp
    postgres_repository_admin.cpp
    postgres_repository_operations.cpp
    postgres_repository_analytics.cpp
    postgres_repository_admin_accounts.cpp
    postgres_seed.cpp
    postgres_seed.h
    postgres_config.h
    postgres_migrations.h
)
add_library(ncs::infrastructure_postgres ALIAS ncs_infrastructure_postgres)

set_target_properties(ncs_infrastructure_postgres PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_infrastructure_postgres PUBLIC cxx_std_17)
target_include_directories(ncs_infrastructure_postgres PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_infrastructure_postgres
    PUBLIC
        ncs::core_application
        OpenSSL::Crypto
        Qt6::Core
        Qt6::Sql
)

ncs_set_project_warnings(ncs_infrastructure_postgres)

add_executable(ncs_sqlite_to_postgres sqlite_to_postgres.cpp)
set_target_properties(ncs_sqlite_to_postgres PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(ncs_sqlite_to_postgres PRIVATE ncs::infrastructure_postgres Qt6::Core Qt6::Sql)
target_compile_definitions(ncs_sqlite_to_postgres PRIVATE
    NCS_POSTGRES_MIGRATIONS_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/migrations"
)
ncs_set_project_warnings(ncs_sqlite_to_postgres)

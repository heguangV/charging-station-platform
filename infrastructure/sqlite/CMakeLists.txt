add_library(ncs_infrastructure_sqlite STATIC
    sqlite_repository.cpp
    sqlite_repository.h
    sqlite_seed.cpp
    sqlite_seed.h
)
add_library(ncs::infrastructure_sqlite ALIAS ncs_infrastructure_sqlite)

set_target_properties(ncs_infrastructure_sqlite PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_infrastructure_sqlite PUBLIC cxx_std_17)
target_include_directories(ncs_infrastructure_sqlite PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_infrastructure_sqlite
    PUBLIC
        ncs::core_application
        OpenSSL::Crypto
        SQLite::SQLite3
)

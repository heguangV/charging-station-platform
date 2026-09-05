add_subdirectory(domain)
add_subdirectory(application)

add_library(ncs_core STATIC
    src/error.cpp
)

target_include_directories(ncs_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ncs_core PUBLIC Qt6::Core)
ncs_set_project_warnings(ncs_core)

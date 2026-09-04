add_library(ncs_core_domain INTERFACE)
add_library(ncs::core_domain ALIAS ncs_core_domain)

target_compile_features(ncs_core_domain INTERFACE cxx_std_17)
target_include_directories(ncs_core_domain INTERFACE ${PROJECT_SOURCE_DIR})

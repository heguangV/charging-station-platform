qt_add_executable(ncs_server
    main.cpp
)

target_link_libraries(ncs_server
    PRIVATE
        ncs_infrastructure
        Qt6::Core
        Qt6::Network
        Qt6::Sql
)

ncs_set_project_warnings(ncs_server)

qt_add_executable(ncs_admin
    admin_main_window.cpp
    admin_main_window.h
    main.cpp
)

target_link_libraries(ncs_admin
    PRIVATE
        ncs_infrastructure
        Qt6::Widgets
        Qt6::Network
)

ncs_set_project_warnings(ncs_admin)

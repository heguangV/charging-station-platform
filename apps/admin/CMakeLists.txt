qt_add_executable(ncs_admin
    admin_api_client.cpp
    admin_api_client.h
    admin_main_window.cpp
    admin_main_window.h
    admin_main_window_data.cpp
    admin_main_window_actions.cpp
    admin_main_window_utils.cpp
    admin_main_window_utils.h
    admin_types.h
    login_widget.cpp
    login_widget.h
    main.cpp
)

target_link_libraries(ncs_admin
    PRIVATE
        ncs_infrastructure
        Qt6::Widgets
        Qt6::Network
)

ncs_set_project_warnings(ncs_admin)

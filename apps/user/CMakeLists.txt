qt_add_executable(ncs_user
    main.cpp
    user_main_window.cpp
    user_main_window.h
)

target_link_libraries(ncs_user
    PRIVATE
        ncs_infrastructure
        Qt6::Widgets
        Qt6::Network
)

ncs_set_project_warnings(ncs_user)

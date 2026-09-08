add_library(ncs_admin_ui STATIC
    admin_api_client.cpp admin_api_client.h
    admin_main_window.cpp admin_main_window.h
    admin_main_window_data.cpp admin_main_window_actions.cpp
    admin_main_window_pages.cpp admin_main_window_session.cpp
    admin_main_window_utils.cpp admin_main_window_utils.h admin_types.h
    admin_page_status.cpp admin_page_status.h
    admin_charts.cpp admin_charts.h login_widget.cpp login_widget.h
)
target_include_directories(ncs_admin_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ncs_admin_ui PUBLIC Qt6::Widgets Qt6::Network)
ncs_set_project_warnings(ncs_admin_ui)

function(ncs_add_admin_resources target)
    qt_add_resources(${target} admin_shared_art
        PREFIX "/admin" BASE "${CMAKE_SOURCE_DIR}/apps/user/resources"
        FILES "${CMAKE_SOURCE_DIR}/apps/user/resources/charging-scene.png")
    qt_add_resources(${target} admin_icons
        PREFIX "/admin" BASE "${CMAKE_SOURCE_DIR}/apps/admin/resources"
        FILES "${CMAKE_SOURCE_DIR}/apps/admin/resources/chevron.svg")
endfunction()
qt_add_executable(ncs_admin main.cpp)
target_link_libraries(ncs_admin PRIVATE ncs_admin_ui ncs_infrastructure)
ncs_add_admin_resources(ncs_admin)
ncs_set_project_warnings(ncs_admin)

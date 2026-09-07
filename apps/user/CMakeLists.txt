qt_add_executable(ncs_user
    main.cpp
    net/api_client.cpp
    net/api_client.h
    net/user_api.cpp
    net/user_api.h
    ui/app_theme.cpp
    ui/app_theme.h
    ui/bottom_navigation.cpp
    ui/bottom_navigation.h
    ui/charge_soc_gauge.cpp
    ui/charge_soc_gauge.h
    ui/charger_table.cpp
    ui/charger_table.h
    ui/station_card.cpp
    ui/station_card.h
    ui/station_list_widget.cpp
    ui/station_list_widget.h
    ui/station_map_widget.cpp
    ui/station_map_widget.h
    user_demo_service.cpp
    user_demo_service.h
    user_main_window.cpp
    user_main_window.h
    user_main_window_login.cpp
    user_main_window_navigation.cpp
    user_main_window_location.cpp
    user_main_window_orders.cpp
    user_main_window_profile.cpp
    user_main_window_state.cpp
)

target_link_libraries(ncs_user
    PRIVATE
        ncs_infrastructure
        Qt6::Widgets
        Qt6::Network
)

if(TARGET Qt6::WebEngineWidgets)
    target_link_libraries(ncs_user PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(ncs_user PRIVATE NCS_HAS_WEBENGINE=1)
endif()

ncs_set_project_warnings(ncs_user)

qt_add_resources(ncs_user user_visual_assets
    PREFIX "/ncs"
    FILES resources/charging-scene.png resources/navigation-chevron.svg
)

find_package(Qt6 ${PROJECT_MINIMUM_QT_VERSION} QUIET COMPONENTS Positioning)
if(TARGET Qt6::Positioning)
    target_link_libraries(ncs_user PRIVATE Qt6::Positioning)
    target_compile_definitions(ncs_user PRIVATE NCS_HAS_POSITIONING=1)
endif()

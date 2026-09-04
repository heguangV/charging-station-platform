add_subdirectory(http)
add_subdirectory(middleware)
add_subdirectory(controller)
add_subdirectory(runtime)
add_subdirectory(websocket)

add_executable(ncs_server
    main.cpp
)

set_target_properties(ncs_server PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_link_libraries(ncs_server PRIVATE
    ncs::crow
    ncs::infrastructure_map
    ncs::infrastructure_sqlite
    ncs::server_controller
    ncs::server_middleware
    ncs::server_runtime
    ncs::server_websocket
)

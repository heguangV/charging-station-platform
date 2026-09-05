add_library(ncs_server_websocket STATIC
    crow_websocket_peer.cpp
    crow_websocket_peer.h
    websocket_routes.cpp
    websocket_routes.h
    outbox_dispatcher.cpp
    outbox_dispatcher.h
    progress_pusher.cpp
    progress_pusher.h
)
add_library(ncs::server_websocket ALIAS ncs_server_websocket)

set_target_properties(ncs_server_websocket PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_server_websocket PUBLIC cxx_std_17)
target_include_directories(ncs_server_websocket PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(ncs_server_websocket
    PUBLIC ncs::core_application
    PUBLIC ncs::crow
    PUBLIC ncs::http_common
    PRIVATE Qt6::Core
)

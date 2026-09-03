qt_add_executable(ncs_foundation_tests
    foundation_tests.cpp
)

target_link_libraries(ncs_foundation_tests
    PRIVATE
        ncs_infrastructure
        Qt6::Core
)

ncs_set_project_warnings(ncs_foundation_tests)

add_test(NAME ncs_foundation_tests COMMAND ncs_foundation_tests)
set_tests_properties(ncs_foundation_tests PROPERTIES LABELS "unit;foundation")

qt_add_executable(ncs_user_net_tests
    user_net_tests.cpp
    ../apps/user/net/api_client.cpp
    ../apps/user/net/api_client.h
)

target_include_directories(ncs_user_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/user)
target_link_libraries(ncs_user_net_tests PRIVATE Qt6::Core Qt6::Network)
ncs_set_project_warnings(ncs_user_net_tests)
add_test(NAME ncs_user_net_tests COMMAND ncs_user_net_tests)
set_tests_properties(ncs_user_net_tests PROPERTIES LABELS "unit;user;network")

add_test(NAME ncs_server_smoke COMMAND ncs_server --smoke-test)
set_tests_properties(ncs_server_smoke PROPERTIES LABELS "smoke;foundation")
add_test(NAME ncs_user_smoke COMMAND ncs_user --smoke-test)
set_tests_properties(ncs_user_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "smoke;ui;foundation"
)
add_test(NAME ncs_admin_smoke COMMAND ncs_admin --smoke-test)
set_tests_properties(ncs_admin_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "smoke;ui;foundation"
)

qt_add_executable(ncs-mobile
    main.cpp
    mapconfiguration.cpp
    mapconfiguration.h
)

qt_add_qml_module(ncs-mobile
    URI Ncs.Mobile
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

qt_add_resources(ncs-mobile ncs_mobile_resources
    PREFIX "/"
    FILES
        resources/tencent-map.html
)

if(ANDROID)
    set(NCS_PROJECT_ROOT_VALUE "")
    set_target_properties(ncs-mobile PROPERTIES
        QT_ANDROID_PACKAGE_NAME io.github.heguangv.chargingstation
        QT_ANDROID_MIN_SDK_VERSION 30
    )
else()
    set(NCS_PROJECT_ROOT_VALUE "${CMAKE_SOURCE_DIR}")
endif()

target_compile_definitions(ncs-mobile PRIVATE
    NCS_PROJECT_ROOT="${NCS_PROJECT_ROOT_VALUE}"
)

target_link_libraries(ncs-mobile PRIVATE
    Qt6::Quick
    Qt6::WebView
)

if(MSVC)
    target_compile_options(ncs-mobile PRIVATE /utf-8)
endif()

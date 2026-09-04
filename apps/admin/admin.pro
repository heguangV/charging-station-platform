QT += core gui widgets network

CONFIG += c++17
TEMPLATE = app
TARGET = ncs_admin

SOURCES += \
    admin_api_client.cpp \
    admin_main_window.cpp \
    admin_main_window_data.cpp \
    login_widget.cpp \
    main.cpp \
    $$PWD/../../core/src/error.cpp \
    $$PWD/../../infrastructure/config/application_config.cpp \
    $$PWD/../../infrastructure/logging/application_logger.cpp

HEADERS += \
    admin_api_client.h \
    admin_main_window.h \
    admin_types.h \
    login_widget.h \
    $$PWD/../../core/include/ncs/core/error.h \
    $$PWD/../../core/include/ncs/core/result.h \
    $$PWD/../../infrastructure/config/application_config.h \
    $$PWD/../../infrastructure/logging/application_logger.h

INCLUDEPATH += \
    $$PWD \
    $$PWD/../../core/include \
    $$PWD/../../infrastructure

unix:!macx {
    QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic
}

DESTDIR = $$OUT_PWD/bin

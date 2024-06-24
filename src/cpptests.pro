TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib c++14
QT -= gui
QT *= network testlib
TARGET = pushpin-cpptest

cpp_build_dir = $$OUT_PWD

MOC_DIR = $$cpp_build_dir/test-moc
OBJECTS_DIR = $$cpp_build_dir/test-obj

SRC_DIR = $$PWD
QZMQ_DIR = $$SRC_DIR/core/qzmq

include($$cpp_build_dir/conf.pri)

INCLUDEPATH += $$SRC_DIR/core
INCLUDEPATH += $$SRC_DIR/proxy
INCLUDEPATH += $$SRC_DIR/handler
INCLUDEPATH += $$SRC_DIR/runner

INCLUDEPATH += $$PWD/../target/include
INCLUDEPATH += $$QZMQ_DIR/src

DEFINES += NO_IRISNET

INCLUDES += \
	$$PWD/core/coretests.h \
	$$PWD/proxy/proxytests.h \
	$$PWD/handler/handlertests.h \
	$$PWD/runner/runnertests.h

SOURCES += \
	$$PWD/core/httpheaderstest.cpp \
	$$PWD/core/jwttest.cpp \
	$$PWD/proxy/routesfiletest.cpp \
	$$PWD/proxy/proxyenginetest.cpp \
	$$PWD/handler/jsonpatchtest.cpp \
	$$PWD/handler/instructtest.cpp \
	$$PWD/handler/idformattest.cpp \
	$$PWD/handler/publishformattest.cpp \
	$$PWD/handler/publishitemtest.cpp \
	$$PWD/handler/handlerenginetest.cpp \
	$$PWD/runner/templatetest.cpp

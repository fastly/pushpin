TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib
QT -= gui
QT *= network testlib
TARGET = pushpin-cpptest
DESTDIR = ..

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

SRC_DIR = $$PWD/..
QZMQ_DIR = $$SRC_DIR/qzmq
RUST_DIR = $$SRC_DIR/../rust

PRE_TARGETDEPS += $$PWD/../libpushpin-cpp.a

include($$PWD/../../../conf.pri)

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$SRC_DIR/proxy
INCLUDEPATH += $$SRC_DIR/handler
INCLUDEPATH += $$SRC_DIR/runner
INCLUDEPATH += $$QZMQ_DIR/src

DEFINES += NO_IRISNET

INCLUDEPATH += $$RUST_DIR/include

INCLUDES += \
	main.h

SOURCES += \
	$$PWD/httpheaderstest/httpheaderstest.cpp \
	$$PWD/jwttest/jwttest.cpp \
	$$PWD/routesfiletest/routesfiletest.cpp \
	$$PWD/proxyenginetest/proxyenginetest.cpp \
	$$PWD/jsonpatchtest/jsonpatchtest.cpp \
	$$PWD/instructtest/instructtest.cpp \
	$$PWD/idformattest/idformattest.cpp \
	$$PWD/publishformattest/publishformattest.cpp \
	$$PWD/publishitemtest/publishitemtest.cpp \
	$$PWD/handlerenginetest/handlerenginetest.cpp \
	$$PWD/templatetest/templatetest.cpp

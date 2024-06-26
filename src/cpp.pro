TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib c++14
QT -= gui
QT += network
TARGET = pushpin-cpp

cpp_build_dir = $$OUT_PWD

MOC_DIR = $$cpp_build_dir/moc
OBJECTS_DIR = $$cpp_build_dir/obj

include($$cpp_build_dir/conf.pri)

SRC_DIR = $$PWD

DEFINES += NO_IRISNET

INCLUDEPATH += $$SRC_DIR/../target/include
INCLUDEPATH += $$SRC_DIR/core
INCLUDEPATH += $$SRC_DIR/core/qzmq/src

include(core/core.pri)
include(m2adapter/m2adapter.pri)
include(proxy/proxy.pri)
include(handler/handler.pri)
include(runner/runner.pri)

TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib c++17
QT -= gui
QT *= network testlib
TARGET = pushpin-cpptest

cpp_build_dir = $$OUT_PWD

MOC_DIR = $$cpp_build_dir/test-moc
OBJECTS_DIR = $$cpp_build_dir/test-obj

include($$cpp_build_dir/conf.pri)

SRC_DIR = $$PWD

DEFINES += NO_IRISNET

INCLUDEPATH += $$SRC_DIR/../target/include
INCLUDEPATH += $$SRC_DIR/core
INCLUDEPATH += /usr/include/hiredis

LIBS += -lhiredis

include(core/tests.pri)
include(proxy/tests.pri)
include(handler/tests.pri)
include(runner/tests.pri)

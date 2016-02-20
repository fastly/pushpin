CONFIG *= console testcase
CONFIG -= app_bundle
QT -= gui
QT *= network testlib

TESTS_DIR = $$PWD
SRC_DIR = $$PWD/../src
CORE_DIR = $$PWD/../../corelib
QZMQ_DIR = $$CORE_DIR/qzmq
COMMON_DIR = $$CORE_DIR/common
DESTDIR = $$TESTS_DIR

LIBS += -L$$SRC_DIR -lpushpin-proxy
PRE_TARGETDEPS += $$PWD/../src/libpushpin-proxy.a
include($$PWD/../conf.pri)

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$CORE_DIR
INCLUDEPATH += $$QZMQ_DIR/src

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

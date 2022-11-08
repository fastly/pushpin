CONFIG *= console testcase
CONFIG -= app_bundle
QT -= gui
QT *= network testlib

TESTS_DIR = $$PWD
SRC_DIR = $$PWD/..
QZMQ_DIR = $$SRC_DIR/qzmq
COMMON_DIR = $$SRC_DIR/common

LIBS += -L$$SRC_DIR -lpushpin-core
PRE_TARGETDEPS += $$PWD/../libpushpin-core.a

include($$PWD/../../rust/lib.pri)
include($$PWD/../../../conf.pri)

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$QZMQ_DIR/src

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

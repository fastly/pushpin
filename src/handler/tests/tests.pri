CONFIG *= console testcase
CONFIG -= app_bundle
QT -= gui
QT *= network testlib

TESTS_DIR = $$PWD
SRC_DIR = $$PWD/..
CPP_DIR = $$PWD/../../cpp
QZMQ_DIR = $$CPP_DIR/qzmq

LIBS += -L$$SRC_DIR -lpushpin-handler
PRE_TARGETDEPS += $$PWD/../libpushpin-handler.a

LIBS += -L$$PWD/../../cpp -lpushpin-cpp
PRE_TARGETDEPS += $$PWD/../../cpp/libpushpin-cpp.a

include($$PWD/../../rust/lib.pri)
include($$PWD/../../../conf.pri)

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$CPP_DIR
INCLUDEPATH += $$QZMQ_DIR/src

DEFINES += NO_IRISNET

CONFIG *= console testcase
CONFIG -= app_bundle
QT -= gui
QT *= network testlib

TESTS_DIR = $$PWD
SRC_DIR = $$PWD/..
CORE_DIR = $$PWD/../../corelib
COMMON_DIR = $$CORE_DIR/common

LIBS += -L$$SRC_DIR -lrunner
PRE_TARGETDEPS += $$PWD/../librunner.a

LIBS += -L$$PWD/../../corelib -lpushpin-core
PRE_TARGETDEPS += $$PWD/../../corelib/libpushpin-core.a

include($$PWD/../../../conf.pri)

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$CORE_DIR

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

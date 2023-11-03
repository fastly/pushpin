CONFIG *= console testcase
CONFIG -= app_bundle
QT -= gui
QT *= network testlib

TESTS_DIR = $$PWD
SRC_DIR = $$PWD/..
CPP_DIR = $$PWD/../../cpp

LIBS += -L$$SRC_DIR -lrunner
PRE_TARGETDEPS += $$PWD/../librunner.a

LIBS += -L$$PWD/../../cpp -lpushpin-cpp
PRE_TARGETDEPS += $$PWD/../../cpp/libpushpin-cpp.a

include($$PWD/../../../conf.pri)

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$CPP_DIR

DEFINES += NO_IRISNET

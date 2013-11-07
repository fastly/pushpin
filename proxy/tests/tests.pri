CONFIG *= console qtestlib testcase
CONFIG -= app_bundle
QT -= gui
QT *= network

TESTS_DIR = $$PWD
SRC_DIR = $$PWD/../src
QZMQ_DIR = $$PWD/../../qzmq
COMMON_DIR = $$PWD/../../common
DESTDIR = $$TESTS_DIR

LIBS += -L$$SRC_DIR -lpushpin-proxy
include($$PWD/../conf.pri)

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$QZMQ_DIR/src

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

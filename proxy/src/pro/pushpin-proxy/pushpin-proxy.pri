SRC_DIR = $$PWD/../..
QZMQ_DIR = $$PWD/../../../../qzmq
COMMON_DIR = $$PWD/../../../../common

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$QZMQ_DIR/src

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

HEADERS += \
	$$SRC_DIR/app.h

SOURCES += \
	$$SRC_DIR/app.cpp \
	$$SRC_DIR/main.cpp

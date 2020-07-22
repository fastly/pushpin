SRC_DIR = $$PWD/..
CORE_DIR = $$PWD/../../corelib
COMMON_DIR = $$CORE_DIR/common

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$CORE_DIR

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

HEADERS += \
	$$SRC_DIR/template.h \
	$$SRC_DIR/service.h \
	$$SRC_DIR/listenport.h \
	$$SRC_DIR/condureservice.h \
	$$SRC_DIR/mongrel2service.h \
	$$SRC_DIR/m2adapterservice.h \
	$$SRC_DIR/zurlservice.h \
	$$SRC_DIR/pushpinproxyservice.h \
	$$SRC_DIR/pushpinhandlerservice.h

SOURCES += \
	$$SRC_DIR/template.cpp \
	$$SRC_DIR/service.cpp \
	$$SRC_DIR/condureservice.cpp \
	$$SRC_DIR/mongrel2service.cpp \
	$$SRC_DIR/m2adapterservice.cpp \
	$$SRC_DIR/zurlservice.cpp \
	$$SRC_DIR/pushpinproxyservice.cpp \
	$$SRC_DIR/pushpinhandlerservice.cpp

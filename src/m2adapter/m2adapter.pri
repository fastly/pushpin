QZMQ_DIR = $$PWD/../corelib/qzmq
COMMON_DIR = $$PWD/../corelib/common

INCLUDEPATH += $$QZMQ_DIR/src

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

HEADERS += \
	$$PWD/m2requestpacket.h \
	$$PWD/m2responsepacket.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/m2requestpacket.cpp \
	$$PWD/m2responsepacket.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp

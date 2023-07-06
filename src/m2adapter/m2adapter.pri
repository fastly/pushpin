CORE_DIR = $$PWD/../corelib
QZMQ_DIR = $$PWD/../corelib/qzmq

INCLUDEPATH += $$CORE_DIR

INCLUDEPATH += $$QZMQ_DIR/src

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

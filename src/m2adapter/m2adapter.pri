CPP_DIR = $$PWD/../cpp
QZMQ_DIR = $$PWD/../cpp/qzmq

INCLUDEPATH += $$CPP_DIR

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

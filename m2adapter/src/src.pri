DEFINES += NO_IRISNET
HEADERS += $$PWD/../../proxy/src/processquit.h
SOURCES += $$PWD/../../proxy/src/processquit.cpp

INCLUDEPATH += $$PWD/../../proxy/src/qzmq/src
include(../../proxy/src/qzmq/src/src.pri)

INCLUDEPATH += $$PWD/../../proxy/src

HEADERS += \
	$$PWD/../../proxy/src/packet/tnetstring.h \
	$$PWD/../../proxy/src/packet/httpheaders.h \
	#$$PWD/packet/httprequestdata.h \
	#$$PWD/packet/httpresponsedata.h \
	$$PWD/../../proxy/src/packet/m2requestpacket.h \
	$$PWD/../../proxy/src/packet/m2responsepacket.h \
	$$PWD/../../proxy/src/packet/zurlrequestpacket.h \
	$$PWD/../../proxy/src/packet/zurlresponsepacket.h \

SOURCES += \
	$$PWD/../../proxy/src/packet/tnetstring.cpp \
	$$PWD/../../proxy/src/packet/httpheaders.cpp \
	$$PWD/../../proxy/src/packet/m2requestpacket.cpp \
	$$PWD/../../proxy/src/packet/m2responsepacket.cpp \
	$$PWD/../../proxy/src/packet/zurlrequestpacket.cpp \
	$$PWD/../../proxy/src/packet/zurlresponsepacket.cpp \

HEADERS += \
	$$PWD/../../proxy/src/log.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/../../proxy/src/log.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp

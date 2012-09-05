INCLUDEPATH += $$PWD/qzmq/src
include(qzmq/src/src.pri)

HEADERS += \
	$$PWD/packet/tnetstring.h \
	$$PWD/packet/httpheaders.h \
	$$PWD/packet/httprequestpacket.h \
	$$PWD/packet/httpresponsepacket.h \
	$$PWD/packet/yurlrequestpacket.h \
	$$PWD/packet/yurlresponsepacket.h \
	$$PWD/packet/inspectrequestpacket.h \
	$$PWD/packet/inspectresponsepacket.h \
	$$PWD/packet/acceptresponsepacket.h

SOURCES += \
	$$PWD/packet/tnetstring.cpp \
	$$PWD/packet/httpheaders.cpp \
	$$PWD/packet/httprequestpacket.cpp \
	$$PWD/packet/httpresponsepacket.cpp \
	$$PWD/packet/yurlrequestpacket.cpp \
	$$PWD/packet/yurlresponsepacket.cpp \
	$$PWD/packet/inspectrequestpacket.cpp \
	$$PWD/packet/inspectresponsepacket.cpp \
	$$PWD/packet/acceptresponsepacket.cpp

HEADERS += \
	$$PWD/app.h

SOURCES += \
	$$PWD/app.cpp \
	$$PWD/main.cpp

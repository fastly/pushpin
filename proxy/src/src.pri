INCLUDEPATH += $$PWD/qzmq/src
include(qzmq/src/src.pri)

HEADERS += \
	$$PWD/packet/tnetstring.h \
	$$PWD/packet/httpheaders.h \
	$$PWD/packet/m2requestpacket.h \
	$$PWD/packet/m2responsepacket.h \
	$$PWD/packet/zurlrequestpacket.h \
	$$PWD/packet/zurlresponsepacket.h \
	$$PWD/packet/inspectrequestpacket.h \
	$$PWD/packet/inspectresponsepacket.h \
	$$PWD/packet/acceptresponsepacket.h

SOURCES += \
	$$PWD/packet/tnetstring.cpp \
	$$PWD/packet/httpheaders.cpp \
	$$PWD/packet/m2requestpacket.cpp \
	$$PWD/packet/m2responsepacket.cpp \
	$$PWD/packet/zurlrequestpacket.cpp \
	$$PWD/packet/zurlresponsepacket.cpp \
	$$PWD/packet/inspectrequestpacket.cpp \
	$$PWD/packet/inspectresponsepacket.cpp \
	$$PWD/packet/acceptresponsepacket.cpp

HEADERS += \
	$$PWD/m2manager.h \
	$$PWD/m2request.h \
	$$PWD/m2response.h \
	$$PWD/zurlmanager.h \
	$$PWD/zurlrequest.h \
	#$$PWD/requestsession.h \
	#$$PWD/proxysession.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/m2manager.cpp \
	$$PWD/m2request.cpp \
	$$PWD/m2response.cpp \
	$$PWD/zurlmanager.cpp \
	$$PWD/zurlrequest.cpp \
	#$$PWD/requestsession.cpp \
	#$$PWD/proxysession.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp

QZMQ_DIR = $$PWD/../../qzmq
COMMON_DIR = $$PWD/../../common

INCLUDEPATH += $$PWD/../../qzmq/src
include(../../qzmq/src/src.pri)

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET
HEADERS += $$COMMON_DIR/processquit.h
SOURCES += $$COMMON_DIR/processquit.cpp

HEADERS += \
	$$COMMON_DIR/tnetstring.h \
	$$COMMON_DIR/httpheaders.h \
	$$COMMON_DIR/log.h

SOURCES += \
	$$COMMON_DIR/tnetstring.cpp \
	$$COMMON_DIR/httpheaders.cpp \
	$$COMMON_DIR/log.cpp

HEADERS += \
	$$PWD/packet/httprequestdata.h \
	$$PWD/packet/httpresponsedata.h \
	$$PWD/packet/m2requestpacket.h \
	$$PWD/packet/m2responsepacket.h \
	$$PWD/packet/zurlrequestpacket.h \
	$$PWD/packet/zurlresponsepacket.h \
	$$PWD/packet/inspectrequestpacket.h \
	$$PWD/packet/inspectresponsepacket.h \
	$$PWD/packet/acceptresponsepacket.h \
	$$PWD/packet/retryrequestpacket.h

SOURCES += \
	$$PWD/packet/m2requestpacket.cpp \
	$$PWD/packet/m2responsepacket.cpp \
	$$PWD/packet/zurlrequestpacket.cpp \
	$$PWD/packet/zurlresponsepacket.cpp \
	$$PWD/packet/inspectrequestpacket.cpp \
	$$PWD/packet/inspectresponsepacket.cpp \
	$$PWD/packet/acceptresponsepacket.cpp \
	$$PWD/packet/retryrequestpacket.cpp

HEADERS += \
	$$PWD/jwt.h \
	$$PWD/layertracker.h \
	$$PWD/m2manager.h \
	$$PWD/m2request.h \
	$$PWD/m2response.h \
	$$PWD/zurlmanager.h \
	$$PWD/zurlrequest.h \
	$$PWD/inspectdata.h \
	$$PWD/inspectmanager.h \
	$$PWD/inspectrequest.h \
	$$PWD/acceptdata.h \
	$$PWD/domainmap.h \
	$$PWD/xffrule.h \
	$$PWD/inspectchecker.h \
	$$PWD/requestsession.h \
	$$PWD/proxysession.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/jwt.cpp \
	$$PWD/layertracker.cpp \
	$$PWD/m2manager.cpp \
	$$PWD/m2request.cpp \
	$$PWD/m2response.cpp \
	$$PWD/zurlmanager.cpp \
	$$PWD/zurlrequest.cpp \
	$$PWD/inspectmanager.cpp \
	$$PWD/inspectrequest.cpp \
	$$PWD/domainmap.cpp \
	$$PWD/inspectchecker.cpp \
	$$PWD/requestsession.cpp \
	$$PWD/proxysession.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp

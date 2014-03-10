SRC_DIR = $$PWD/../..
QZMQ_DIR = $$PWD/../../../../qzmq
COMMON_DIR = $$PWD/../../../../common

INCLUDEPATH += $$SRC_DIR

INCLUDEPATH += $$QZMQ_DIR/src
include($$QZMQ_DIR/src/src.pri)

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET
HEADERS += $$COMMON_DIR/processquit.h
SOURCES += $$COMMON_DIR/processquit.cpp

HEADERS += \
	$$COMMON_DIR/tnetstring.h \
	$$COMMON_DIR/httpheaders.h \
	$$COMMON_DIR/zhttprequestpacket.h \
	$$COMMON_DIR/zhttpresponsepacket.h \
	$$COMMON_DIR/log.h \
	$$COMMON_DIR/bufferlist.h

SOURCES += \
	$$COMMON_DIR/tnetstring.cpp \
	$$COMMON_DIR/httpheaders.cpp \
	$$COMMON_DIR/zhttprequestpacket.cpp \
	$$COMMON_DIR/zhttpresponsepacket.cpp \
	$$COMMON_DIR/log.cpp \
	$$COMMON_DIR/bufferlist.cpp

HEADERS += \
	$$SRC_DIR/packet/httprequestdata.h \
	$$SRC_DIR/packet/httpresponsedata.h \
	$$SRC_DIR/packet/inspectrequestpacket.h \
	$$SRC_DIR/packet/inspectresponsepacket.h \
	$$SRC_DIR/packet/acceptresponsepacket.h \
	$$SRC_DIR/packet/retryrequestpacket.h \
	$$SRC_DIR/packet/wscontrolpacket.h

SOURCES += \
	$$SRC_DIR/packet/inspectrequestpacket.cpp \
	$$SRC_DIR/packet/inspectresponsepacket.cpp \
	$$SRC_DIR/packet/acceptresponsepacket.cpp \
	$$SRC_DIR/packet/retryrequestpacket.cpp \
	$$SRC_DIR/packet/wscontrolpacket.cpp

HEADERS += \
	$$SRC_DIR/jwt.h \
	$$SRC_DIR/layertracker.h \
	$$SRC_DIR/zhttpmanager.h \
	$$SRC_DIR/zhttprequest.h \
	$$SRC_DIR/zwebsocket.h \
	$$SRC_DIR/inspectdata.h \
	$$SRC_DIR/inspectmanager.h \
	$$SRC_DIR/inspectrequest.h \
	$$SRC_DIR/wscontrolmanager.h \
	$$SRC_DIR/wscontrolsession.h \
	$$SRC_DIR/acceptdata.h \
	$$SRC_DIR/domainmap.h \
	$$SRC_DIR/xffrule.h \
	$$SRC_DIR/inspectchecker.h \
	$$SRC_DIR/requestsession.h \
	$$SRC_DIR/proxyutil.h \
	$$SRC_DIR/proxysession.h \
	$$SRC_DIR/wsproxysession.h \
	$$SRC_DIR/engine.h

SOURCES += \
	$$SRC_DIR/jwt.cpp \
	$$SRC_DIR/layertracker.cpp \
	$$SRC_DIR/zhttpmanager.cpp \
	$$SRC_DIR/zhttprequest.cpp \
	$$SRC_DIR/zwebsocket.cpp \
	$$SRC_DIR/inspectmanager.cpp \
	$$SRC_DIR/inspectrequest.cpp \
	$$SRC_DIR/wscontrolmanager.cpp \
	$$SRC_DIR/wscontrolsession.cpp \
	$$SRC_DIR/domainmap.cpp \
	$$SRC_DIR/inspectchecker.cpp \
	$$SRC_DIR/requestsession.cpp \
	$$SRC_DIR/proxyutil.cpp \
	$$SRC_DIR/proxysession.cpp \
	$$SRC_DIR/wsproxysession.cpp \
	$$SRC_DIR/engine.cpp

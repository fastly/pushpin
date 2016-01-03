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
	$$COMMON_DIR/bufferlist.h \
	$$COMMON_DIR/layertracker.h

SOURCES += \
	$$COMMON_DIR/tnetstring.cpp \
	$$COMMON_DIR/httpheaders.cpp \
	$$COMMON_DIR/zhttprequestpacket.cpp \
	$$COMMON_DIR/zhttpresponsepacket.cpp \
	$$COMMON_DIR/log.cpp \
	$$COMMON_DIR/bufferlist.cpp \
	$$COMMON_DIR/layertracker.cpp

HEADERS += \
	$$SRC_DIR/packet/httprequestdata.h \
	$$SRC_DIR/packet/httpresponsedata.h \
	$$SRC_DIR/packet/retryrequestpacket.h \
	$$SRC_DIR/packet/wscontrolpacket.h \
	$$SRC_DIR/packet/statspacket.h \
	$$SRC_DIR/packet/zrpcrequestpacket.h \
	$$SRC_DIR/packet/zrpcresponsepacket.h

SOURCES += \
	$$SRC_DIR/packet/retryrequestpacket.cpp \
	$$SRC_DIR/packet/wscontrolpacket.cpp \
	$$SRC_DIR/packet/statspacket.cpp \
	$$SRC_DIR/packet/zrpcrequestpacket.cpp \
	$$SRC_DIR/packet/zrpcresponsepacket.cpp

HEADERS += \
	$$SRC_DIR/uuidutil.h \
	$$SRC_DIR/zutil.h \
	$$SRC_DIR/jwt.h \
	$$SRC_DIR/websocket.h \
	$$SRC_DIR/zhttpmanager.h \
	$$SRC_DIR/zhttprequest.h \
	$$SRC_DIR/zwebsocket.h \
	$$SRC_DIR/websocketoverhttp.h \
	$$SRC_DIR/zrpcmanager.h \
	$$SRC_DIR/zrpcrequest.h \
	$$SRC_DIR/zrpcchecker.h \
	$$SRC_DIR/sockjsmanager.h \
	$$SRC_DIR/sockjssession.h \
	$$SRC_DIR/inspectdata.h \
	$$SRC_DIR/inspectrequest.h \
	$$SRC_DIR/acceptrequest.h \
	$$SRC_DIR/connectionmanager.h \
	$$SRC_DIR/wscontrolmanager.h \
	$$SRC_DIR/wscontrolsession.h \
	$$SRC_DIR/acceptdata.h \
	$$SRC_DIR/domainmap.h \
	$$SRC_DIR/zroutes.h \
	$$SRC_DIR/xffrule.h \
	$$SRC_DIR/cors.h \
	$$SRC_DIR/requestsession.h \
	$$SRC_DIR/proxyutil.h \
	$$SRC_DIR/proxysession.h \
	$$SRC_DIR/wsproxysession.h \
	$$SRC_DIR/statsmanager.h \
	$$SRC_DIR/updater.h \
	$$SRC_DIR/engine.h

SOURCES += \
	$$SRC_DIR/uuidutil.cpp \
	$$SRC_DIR/zutil.cpp \
	$$SRC_DIR/jwt.cpp \
	$$SRC_DIR/zhttpmanager.cpp \
	$$SRC_DIR/zhttprequest.cpp \
	$$SRC_DIR/zwebsocket.cpp \
	$$SRC_DIR/websocketoverhttp.cpp \
	$$SRC_DIR/zrpcmanager.cpp \
	$$SRC_DIR/zrpcrequest.cpp \
	$$SRC_DIR/zrpcchecker.cpp \
	$$SRC_DIR/sockjsmanager.cpp \
	$$SRC_DIR/sockjssession.cpp \
	$$SRC_DIR/inspectrequest.cpp \
	$$SRC_DIR/acceptrequest.cpp \
	$$SRC_DIR/connectionmanager.cpp \
	$$SRC_DIR/wscontrolmanager.cpp \
	$$SRC_DIR/wscontrolsession.cpp \
	$$SRC_DIR/domainmap.cpp \
	$$SRC_DIR/zroutes.cpp \
	$$SRC_DIR/cors.cpp \
	$$SRC_DIR/requestsession.cpp \
	$$SRC_DIR/proxyutil.cpp \
	$$SRC_DIR/proxysession.cpp \
	$$SRC_DIR/wsproxysession.cpp \
	$$SRC_DIR/statsmanager.cpp \
	$$SRC_DIR/updater.cpp \
	$$SRC_DIR/engine.cpp

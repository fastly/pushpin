SRC_DIR = $$PWD/../..
CORE_DIR = $$PWD/../../../../corelib
QZMQ_DIR = $$CORE_DIR/qzmq
COMMON_DIR = $$CORE_DIR/common

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$CORE_DIR

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
	$$CORE_DIR/packet/httprequestdata.h \
	$$CORE_DIR/packet/httpresponsedata.h \
	$$CORE_DIR/packet/retryrequestpacket.h \
	$$CORE_DIR/packet/wscontrolpacket.h \
	$$CORE_DIR/packet/statspacket.h \
	$$CORE_DIR/packet/zrpcrequestpacket.h \
	$$CORE_DIR/packet/zrpcresponsepacket.h

SOURCES += \
	$$CORE_DIR/packet/retryrequestpacket.cpp \
	$$CORE_DIR/packet/wscontrolpacket.cpp \
	$$CORE_DIR/packet/statspacket.cpp \
	$$CORE_DIR/packet/zrpcrequestpacket.cpp \
	$$CORE_DIR/packet/zrpcresponsepacket.cpp

HEADERS += \
	$$CORE_DIR/uuidutil.h \
	$$CORE_DIR/zutil.h \
	$$CORE_DIR/websocket.h \
	$$CORE_DIR/zhttpmanager.h \
	$$CORE_DIR/zhttprequest.h \
	$$CORE_DIR/zwebsocket.h \
	$$CORE_DIR/zrpcmanager.h \
	$$CORE_DIR/zrpcrequest.h \
	$$CORE_DIR/inspectdata.h \
	$$CORE_DIR/cors.h \
	$$CORE_DIR/statsmanager.h

SOURCES += \
	$$CORE_DIR/uuidutil.cpp \
	$$CORE_DIR/zutil.cpp \
	$$CORE_DIR/zhttpmanager.cpp \
	$$CORE_DIR/zhttprequest.cpp \
	$$CORE_DIR/zwebsocket.cpp \
	$$CORE_DIR/zrpcmanager.cpp \
	$$CORE_DIR/zrpcrequest.cpp \
	$$CORE_DIR/cors.cpp \
	$$CORE_DIR/statsmanager.cpp

HEADERS += \
	$$SRC_DIR/jwt.h \
	$$SRC_DIR/websocketoverhttp.h \
	$$SRC_DIR/zrpcchecker.h \
	$$SRC_DIR/sockjsmanager.h \
	$$SRC_DIR/sockjssession.h \
	$$SRC_DIR/inspectrequest.h \
	$$SRC_DIR/acceptrequest.h \
	$$SRC_DIR/connectionmanager.h \
	$$SRC_DIR/wscontrolmanager.h \
	$$SRC_DIR/wscontrolsession.h \
	$$SRC_DIR/acceptdata.h \
	$$SRC_DIR/domainmap.h \
	$$SRC_DIR/zroutes.h \
	$$SRC_DIR/xffrule.h \
	$$SRC_DIR/requestsession.h \
	$$SRC_DIR/proxyutil.h \
	$$SRC_DIR/proxysession.h \
	$$SRC_DIR/wsproxysession.h \
	$$SRC_DIR/updater.h \
	$$SRC_DIR/engine.h

SOURCES += \
	$$SRC_DIR/jwt.cpp \
	$$SRC_DIR/websocketoverhttp.cpp \
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
	$$SRC_DIR/requestsession.cpp \
	$$SRC_DIR/proxyutil.cpp \
	$$SRC_DIR/proxysession.cpp \
	$$SRC_DIR/wsproxysession.cpp \
	$$SRC_DIR/updater.cpp \
	$$SRC_DIR/engine.cpp

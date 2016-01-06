SRC_DIR = $$PWD/../..
QZMQ_DIR = $$PWD/../../../../corelib/qzmq
COMMON_DIR = $$PWD/../../../../corelib/common
PROXY_SRC_DIR = $$PWD/../../../../proxy/src

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$PROXY_SRC_DIR

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
	$$PROXY_SRC_DIR/packet/httprequestdata.h \
	$$PROXY_SRC_DIR/packet/httpresponsedata.h \
	$$PROXY_SRC_DIR/packet/inspectdata.h \
	$$PROXY_SRC_DIR/packet/zrpcrequestpacket.h \
	$$PROXY_SRC_DIR/packet/zrpcresponsepacket.h \
	$$PROXY_SRC_DIR/packet/retryrequestpacket.h \
	$$PROXY_SRC_DIR/packet/wscontrolpacket.h \
	$$PROXY_SRC_DIR/packet/statspacket.h \
	$$PROXY_SRC_DIR/uuidutil.h \
	$$PROXY_SRC_DIR/zutil.h \
	$$PROXY_SRC_DIR/zrpcmanager.h \
	$$PROXY_SRC_DIR/zrpcrequest.h \
	$$PROXY_SRC_DIR/websocket.h \
	$$PROXY_SRC_DIR/zhttpmanager.h \
	$$PROXY_SRC_DIR/zhttprequest.h \
	$$PROXY_SRC_DIR/zwebsocket.h \
	$$PROXY_SRC_DIR/cors.h \
	$$PROXY_SRC_DIR/statsmanager.h \
	$$SRC_DIR/deferred.h \
	$$SRC_DIR/statusreasons.h \
	$$SRC_DIR/httpserver.h \
	$$SRC_DIR/jsonpointer.h \
	$$SRC_DIR/jsonpatch.h \
	$$SRC_DIR/engine.h

SOURCES += \
	$$PROXY_SRC_DIR/packet/zrpcrequestpacket.cpp \
	$$PROXY_SRC_DIR/packet/zrpcresponsepacket.cpp \
	$$PROXY_SRC_DIR/packet/retryrequestpacket.cpp \
	$$PROXY_SRC_DIR/packet/wscontrolpacket.cpp \
	$$PROXY_SRC_DIR/packet/statspacket.cpp \
	$$PROXY_SRC_DIR/uuidutil.cpp \
	$$PROXY_SRC_DIR/zutil.cpp \
	$$PROXY_SRC_DIR/zrpcmanager.cpp \
	$$PROXY_SRC_DIR/zrpcrequest.cpp \
	$$PROXY_SRC_DIR/zhttpmanager.cpp \
	$$PROXY_SRC_DIR/zhttprequest.cpp \
	$$PROXY_SRC_DIR/zwebsocket.cpp \
	$$PROXY_SRC_DIR/cors.cpp \
	$$PROXY_SRC_DIR/statsmanager.cpp \
	$$SRC_DIR/deferred.cpp \
	$$SRC_DIR/statusreasons.cpp \
	$$SRC_DIR/httpserver.cpp \
	$$SRC_DIR/jsonpointer.cpp \
	$$SRC_DIR/jsonpatch.cpp \
	$$SRC_DIR/engine.cpp

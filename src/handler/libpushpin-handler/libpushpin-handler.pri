SRC_DIR = $$PWD/..
CORE_DIR = $$PWD/../../corelib
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
	$$CORE_DIR/packet/zrpcrequestpacket.h \
	$$CORE_DIR/packet/zrpcresponsepacket.h \
	$$CORE_DIR/packet/retryrequestpacket.h \
	$$CORE_DIR/packet/wscontrolpacket.h \
	$$CORE_DIR/packet/statspacket.h

SOURCES += \
	$$CORE_DIR/packet/zrpcrequestpacket.cpp \
	$$CORE_DIR/packet/zrpcresponsepacket.cpp \
	$$CORE_DIR/packet/retryrequestpacket.cpp \
	$$CORE_DIR/packet/wscontrolpacket.cpp \
	$$CORE_DIR/packet/statspacket.cpp

HEADERS += \
	$$CORE_DIR/inspectdata.h \
	$$CORE_DIR/uuidutil.h \
	$$CORE_DIR/zutil.h \
	$$CORE_DIR/zrpcmanager.h \
	$$CORE_DIR/zrpcrequest.h \
	$$CORE_DIR/websocket.h \
	$$CORE_DIR/zhttpmanager.h \
	$$CORE_DIR/zhttprequest.h \
	$$CORE_DIR/zwebsocket.h \
	$$CORE_DIR/cors.h \
	$$CORE_DIR/statsmanager.h

SOURCES += \
	$$CORE_DIR/uuidutil.cpp \
	$$CORE_DIR/zutil.cpp \
	$$CORE_DIR/zrpcmanager.cpp \
	$$CORE_DIR/zrpcrequest.cpp \
	$$CORE_DIR/zhttpmanager.cpp \
	$$CORE_DIR/zhttprequest.cpp \
	$$CORE_DIR/zwebsocket.cpp \
	$$CORE_DIR/cors.cpp \
	$$CORE_DIR/statsmanager.cpp

HEADERS += \
	$$SRC_DIR/deferred.h \
	$$SRC_DIR/statusreasons.h \
	$$SRC_DIR/httpserver.h \
	$$SRC_DIR/variantutil.h \
	$$SRC_DIR/jsonpointer.h \
	$$SRC_DIR/jsonpatch.h \
	$$SRC_DIR/detectrule.h \
	$$SRC_DIR/lastids.h \
	$$SRC_DIR/cidset.h \
	$$SRC_DIR/sessionrequest.h \
	$$SRC_DIR/requeststate.h \
	$$SRC_DIR/wscontrolmessage.h \
	$$SRC_DIR/publishformat.h \
	$$SRC_DIR/publishitem.h \
	$$SRC_DIR/instruct.h \
	$$SRC_DIR/responselastids.h \
	$$SRC_DIR/controlrequest.h \
	$$SRC_DIR/conncheckworker.h \
	$$SRC_DIR/engine.h

SOURCES += \
	$$SRC_DIR/deferred.cpp \
	$$SRC_DIR/statusreasons.cpp \
	$$SRC_DIR/httpserver.cpp \
	$$SRC_DIR/variantutil.cpp \
	$$SRC_DIR/jsonpointer.cpp \
	$$SRC_DIR/jsonpatch.cpp \
	$$SRC_DIR/sessionrequest.cpp \
	$$SRC_DIR/requeststate.cpp \
	$$SRC_DIR/wscontrolmessage.cpp \
	$$SRC_DIR/publishformat.cpp \
	$$SRC_DIR/publishitem.cpp \
	$$SRC_DIR/instruct.cpp \
	$$SRC_DIR/responselastids.cpp \
	$$SRC_DIR/controlrequest.cpp \
	$$SRC_DIR/conncheckworker.cpp \
	$$SRC_DIR/engine.cpp

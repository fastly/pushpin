QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_CFLAGS += $$(CFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)

SRC_DIR = $$PWD/..
QZMQ_DIR = $$SRC_DIR/qzmq
COMMON_DIR = $$SRC_DIR/common
RUST_DIR = $$SRC_DIR/../rust

INCLUDEPATH += $$QZMQ_DIR/src
include($$QZMQ_DIR/src/src.pri)

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET
HEADERS += $$COMMON_DIR/processquit.h
SOURCES += $$COMMON_DIR/processquit.cpp

INCLUDEPATH += $$RUST_DIR/include

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
	$$SRC_DIR/callback.h \
	$$SRC_DIR/timerwheel.h \
	$$SRC_DIR/jwt.h \
	$$SRC_DIR/rtimer.h \
	$$SRC_DIR/logutil.h \
	$$SRC_DIR/uuidutil.h \
	$$SRC_DIR/zutil.h \
	$$SRC_DIR/httprequest.h \
	$$SRC_DIR/websocket.h \
	$$SRC_DIR/zhttpmanager.h \
	$$SRC_DIR/zhttprequest.h \
	$$SRC_DIR/zwebsocket.h \
	$$SRC_DIR/zrpcmanager.h \
	$$SRC_DIR/zrpcrequest.h \
	$$SRC_DIR/statusreasons.h \
	$$SRC_DIR/inspectdata.h \
	$$SRC_DIR/cors.h \
	$$SRC_DIR/simplehttpserver.h \
	$$SRC_DIR/stats.h \
	$$SRC_DIR/statsmanager.h \
	$$SRC_DIR/settings.h

SOURCES += \
	$$SRC_DIR/timerwheel.cpp \
	$$SRC_DIR/jwt.cpp \
	$$SRC_DIR/rtimer.cpp \
	$$SRC_DIR/logutil.cpp \
	$$SRC_DIR/uuidutil.cpp \
	$$SRC_DIR/zutil.cpp \
	$$SRC_DIR/zhttpmanager.cpp \
	$$SRC_DIR/zhttprequest.cpp \
	$$SRC_DIR/zwebsocket.cpp \
	$$SRC_DIR/zrpcmanager.cpp \
	$$SRC_DIR/zrpcrequest.cpp \
	$$SRC_DIR/statusreasons.cpp \
	$$SRC_DIR/cors.cpp \
	$$SRC_DIR/simplehttpserver.cpp \
	$$SRC_DIR/stats.cpp \
	$$SRC_DIR/statsmanager.cpp \
	$$SRC_DIR/settings.cpp

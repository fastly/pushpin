QZMQ_DIR = $$PWD/qzmq
COMMON_DIR = $$PWD/common
RUST_DIR = $$PWD/../rust

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
	$$PWD/packet/httprequestdata.h \
	$$PWD/packet/httpresponsedata.h \
	$$PWD/packet/retryrequestpacket.h \
	$$PWD/packet/wscontrolpacket.h \
	$$PWD/packet/statspacket.h \
	$$PWD/packet/zrpcrequestpacket.h \
	$$PWD/packet/zrpcresponsepacket.h

SOURCES += \
	$$PWD/packet/retryrequestpacket.cpp \
	$$PWD/packet/wscontrolpacket.cpp \
	$$PWD/packet/statspacket.cpp \
	$$PWD/packet/zrpcrequestpacket.cpp \
	$$PWD/packet/zrpcresponsepacket.cpp

HEADERS += \
	$$PWD/timerwheel.h \
	$$PWD/rtimer.h \
	$$PWD/logutil.h \
	$$PWD/uuidutil.h \
	$$PWD/zutil.h \
	$$PWD/httprequest.h \
	$$PWD/websocket.h \
	$$PWD/zhttpmanager.h \
	$$PWD/zhttprequest.h \
	$$PWD/zwebsocket.h \
	$$PWD/zrpcmanager.h \
	$$PWD/zrpcrequest.h \
	$$PWD/statusreasons.h \
	$$PWD/inspectdata.h \
	$$PWD/cors.h \
	$$PWD/simplehttpserver.h \
	$$PWD/statsmanager.h \
	$$PWD/settings.h

SOURCES += \
	$$PWD/timerwheel.cpp \
	$$PWD/rtimer.cpp \
	$$PWD/logutil.cpp \
	$$PWD/uuidutil.cpp \
	$$PWD/zutil.cpp \
	$$PWD/zhttpmanager.cpp \
	$$PWD/zhttprequest.cpp \
	$$PWD/zwebsocket.cpp \
	$$PWD/zrpcmanager.cpp \
	$$PWD/zrpcrequest.cpp \
	$$PWD/statusreasons.cpp \
	$$PWD/cors.cpp \
	$$PWD/simplehttpserver.cpp \
	$$PWD/statsmanager.cpp \
	$$PWD/settings.cpp

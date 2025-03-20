HEADERS += \
	$$PWD/qzmqcontext.h \
	$$PWD/qzmqsocket.h \
	$$PWD/qzmqvalve.h \
	$$PWD/qzmqreqmessage.h \
	$$PWD/qzmqreprouter.h

SOURCES += \
	$$PWD/qzmqcontext.cpp \
	$$PWD/qzmqsocket.cpp \
	$$PWD/qzmqvalve.cpp \
	$$PWD/qzmqreprouter.cpp

HEADERS += $$PWD/processquit.h
SOURCES += $$PWD/processquit.cpp

HEADERS += \
	$$PWD/test.h \
	$$PWD/tnetstring.h \
	$$PWD/httpheaders.h \
	$$PWD/zhttprequestpacket.h \
	$$PWD/zhttpresponsepacket.h \
	$$PWD/log.h \
	$$PWD/bufferlist.h \
	$$PWD/layertracker.h

SOURCES += \
	$$PWD/tnetstring.cpp \
	$$PWD/httpheaders.cpp \
	$$PWD/zhttprequestpacket.cpp \
	$$PWD/zhttpresponsepacket.cpp \
	$$PWD/log.cpp \
	$$PWD/bufferlist.cpp \
	$$PWD/layertracker.cpp

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
	$$PWD/callback.h \
	$$PWD/config.h \
	$$PWD/timerwheel.h \
	$$PWD/jwt.h \
	$$PWD/timer.h \
	$$PWD/defercall.h \
	$$PWD/socketnotifier.h \
	$$PWD/eventloop.h \
	$$PWD/readwrite.h \
	$$PWD/tcplistener.h \
	$$PWD/tcpstream.h \
	$$PWD/unixlistener.h \
	$$PWD/unixstream.h \
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
	$$PWD/stats.h \
	$$PWD/statsmanager.h \
	$$PWD/settings.h

SOURCES += \
	$$PWD/config.cpp \
	$$PWD/timerwheel.cpp \
	$$PWD/jwt.cpp \
	$$PWD/timer.cpp \
	$$PWD/defercall.cpp \
	$$PWD/socketnotifier.cpp \
	$$PWD/eventloop.cpp \
	$$PWD/tcplistener.cpp \
	$$PWD/tcpstream.cpp \
	$$PWD/unixlistener.cpp \
	$$PWD/unixstream.cpp \
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
	$$PWD/stats.cpp \
	$$PWD/statsmanager.cpp \
	$$PWD/settings.cpp

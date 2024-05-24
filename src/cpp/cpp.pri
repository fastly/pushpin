QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_CFLAGS += $$(CFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)

SRC_DIR = $$PWD
QZMQ_DIR = $$SRC_DIR/qzmq
RUST_DIR = $$SRC_DIR/..

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$QZMQ_DIR/src
include($$QZMQ_DIR/src/src.pri)

DEFINES += NO_IRISNET
HEADERS += $$SRC_DIR/processquit.h
SOURCES += $$SRC_DIR/processquit.cpp

INCLUDEPATH += $$RUST_DIR

HEADERS += \
	$$SRC_DIR/tnetstring.h \
	$$SRC_DIR/httpheaders.h \
	$$SRC_DIR/zhttprequestpacket.h \
	$$SRC_DIR/zhttpresponsepacket.h \
	$$SRC_DIR/log.h \
	$$SRC_DIR/bufferlist.h \
	$$SRC_DIR/layertracker.h

SOURCES += \
	$$SRC_DIR/tnetstring.cpp \
	$$SRC_DIR/httpheaders.cpp \
	$$SRC_DIR/zhttprequestpacket.cpp \
	$$SRC_DIR/zhttpresponsepacket.cpp \
	$$SRC_DIR/log.cpp \
	$$SRC_DIR/bufferlist.cpp \
	$$SRC_DIR/layertracker.cpp

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
	$$SRC_DIR/config.h \
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
	$$SRC_DIR/config.cpp \
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

PSRC_DIR = $$SRC_DIR/proxy

HEADERS += \
	$$PSRC_DIR/testhttprequest.h \
	$$PSRC_DIR/testwebsocket.h \
	$$PSRC_DIR/websocketoverhttp.h \
	$$PSRC_DIR/zrpcchecker.h \
	$$PSRC_DIR/sockjsmanager.h \
	$$PSRC_DIR/sockjssession.h \
	$$PSRC_DIR/inspectrequest.h \
	$$PSRC_DIR/acceptrequest.h \
	$$PSRC_DIR/connectionmanager.h \
	$$PSRC_DIR/wscontrolmanager.h \
	$$PSRC_DIR/wscontrolsession.h \
	$$PSRC_DIR/acceptdata.h \
	$$PSRC_DIR/routesfile.h \
	$$PSRC_DIR/domainmap.h \
	$$PSRC_DIR/zroutes.h \
	$$PSRC_DIR/xffrule.h \
	$$PSRC_DIR/requestsession.h \
	$$PSRC_DIR/proxyutil.h \
	$$PSRC_DIR/proxysession.h \
	$$PSRC_DIR/wsproxysession.h \
	$$PSRC_DIR/updater.h \
	$$PSRC_DIR/engine.h \
	$$PSRC_DIR/app.h \
	$$PSRC_DIR/main.h

SOURCES += \
	$$PSRC_DIR/testhttprequest.cpp \
	$$PSRC_DIR/testwebsocket.cpp \
	$$PSRC_DIR/websocketoverhttp.cpp \
	$$PSRC_DIR/zrpcchecker.cpp \
	$$PSRC_DIR/sockjsmanager.cpp \
	$$PSRC_DIR/sockjssession.cpp \
	$$PSRC_DIR/inspectrequest.cpp \
	$$PSRC_DIR/acceptrequest.cpp \
	$$PSRC_DIR/connectionmanager.cpp \
	$$PSRC_DIR/wscontrolmanager.cpp \
	$$PSRC_DIR/wscontrolsession.cpp \
	$$PSRC_DIR/routesfile.cpp \
	$$PSRC_DIR/domainmap.cpp \
	$$PSRC_DIR/zroutes.cpp \
	$$PSRC_DIR/requestsession.cpp \
	$$PSRC_DIR/proxyutil.cpp \
	$$PSRC_DIR/proxysession.cpp \
	$$PSRC_DIR/wsproxysession.cpp \
	$$PSRC_DIR/updater.cpp \
	$$PSRC_DIR/engine.cpp \
	$$PSRC_DIR/app.cpp \
	$$PSRC_DIR/main.cpp

HSRC_DIR = $$SRC_DIR/handler

HEADERS += \
	$$HSRC_DIR/deferred.h \
	$$HSRC_DIR/variantutil.h \
	$$HSRC_DIR/jsonpointer.h \
	$$HSRC_DIR/jsonpatch.h \
	$$HSRC_DIR/detectrule.h \
	$$HSRC_DIR/lastids.h \
	$$HSRC_DIR/cidset.h \
	$$HSRC_DIR/sessionrequest.h \
	$$HSRC_DIR/requeststate.h \
	$$HSRC_DIR/wscontrolmessage.h \
	$$HSRC_DIR/publishformat.h \
	$$HSRC_DIR/publishitem.h \
	$$HSRC_DIR/instruct.h \
	$$HSRC_DIR/format.h \
	$$HSRC_DIR/idformat.h \
	$$HSRC_DIR/httpsession.h \
	$$HSRC_DIR/httpsessionupdatemanager.h \
	$$HSRC_DIR/wssession.h \
	$$HSRC_DIR/publishlastids.h \
	$$HSRC_DIR/controlrequest.h \
	$$HSRC_DIR/conncheckworker.h \
	$$HSRC_DIR/refreshworker.h \
	$$HSRC_DIR/ratelimiter.h \
	$$HSRC_DIR/sequencer.h \
	$$HSRC_DIR/filter.h \
	$$HSRC_DIR/filterstack.h \
	$$HSRC_DIR/handlerengine.h \
	$$HSRC_DIR/handlerapp.h \
	$$HSRC_DIR/main.h

SOURCES += \
	$$HSRC_DIR/deferred.cpp \
	$$HSRC_DIR/variantutil.cpp \
	$$HSRC_DIR/jsonpointer.cpp \
	$$HSRC_DIR/jsonpatch.cpp \
	$$HSRC_DIR/sessionrequest.cpp \
	$$HSRC_DIR/requeststate.cpp \
	$$HSRC_DIR/wscontrolmessage.cpp \
	$$HSRC_DIR/publishformat.cpp \
	$$HSRC_DIR/publishitem.cpp \
	$$HSRC_DIR/instruct.cpp \
	$$HSRC_DIR/format.cpp \
	$$HSRC_DIR/idformat.cpp \
	$$HSRC_DIR/httpsession.cpp \
	$$HSRC_DIR/httpsessionupdatemanager.cpp \
	$$HSRC_DIR/wssession.cpp \
	$$HSRC_DIR/publishlastids.cpp \
	$$HSRC_DIR/controlrequest.cpp \
	$$HSRC_DIR/conncheckworker.cpp \
	$$HSRC_DIR/refreshworker.cpp \
	$$HSRC_DIR/ratelimiter.cpp \
	$$HSRC_DIR/sequencer.cpp \
	$$HSRC_DIR/filter.cpp \
	$$HSRC_DIR/filterstack.cpp \
	$$HSRC_DIR/handlerengine.cpp \
	$$HSRC_DIR/handlerapp.cpp \
	$$HSRC_DIR/handlermain.cpp

MSRC_DIR = $$SRC_DIR/m2adapter

HEADERS += \
	$$MSRC_DIR/m2requestpacket.h \
        $$MSRC_DIR/m2responsepacket.h \
        $$MSRC_DIR/m2adapterapp.h \
	$$MSRC_DIR/main.h

SOURCES += \
	$$MSRC_DIR/m2requestpacket.cpp \
	$$MSRC_DIR/m2responsepacket.cpp \
	$$MSRC_DIR/m2adapterapp.cpp \
	$$MSRC_DIR/m2adaptermain.cpp

RSRC_DIR = $$SRC_DIR/runner

HEADERS += \
	$$RSRC_DIR/template.h \
	$$RSRC_DIR/service.h \
	$$RSRC_DIR/listenport.h \
	$$RSRC_DIR/connmgrservice.h \
	$$RSRC_DIR/mongrel2service.h \
	$$RSRC_DIR/m2adapterservice.h \
	$$RSRC_DIR/zurlservice.h \
	$$RSRC_DIR/pushpinproxyservice.h \
	$$RSRC_DIR/pushpinhandlerservice.h \
	$$RSRC_DIR/runnerapp.h \
	$$RSRC_DIR/main.h

SOURCES += \
	$$RSRC_DIR/template.cpp \
	$$RSRC_DIR/service.cpp \
	$$RSRC_DIR/connmgrservice.cpp \
	$$RSRC_DIR/mongrel2service.cpp \
	$$RSRC_DIR/m2adapterservice.cpp \
	$$RSRC_DIR/zurlservice.cpp \
	$$RSRC_DIR/pushpinproxyservice.cpp \
	$$RSRC_DIR/pushpinhandlerservice.cpp \
	$$RSRC_DIR/runnerapp.cpp \
	$$RSRC_DIR/runnermain.cpp

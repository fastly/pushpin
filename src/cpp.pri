QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_CFLAGS += $$(CFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)

INCLUDEPATH += $$PWD/../target/include

SRC_DIR = $$PWD
QZMQ_DIR = $$SRC_DIR/core/qzmq

INCLUDEPATH += $$SRC_DIR/core
INCLUDEPATH += $$QZMQ_DIR/src
include($$QZMQ_DIR/src/src.pri)

CSRC_DIR = $$SRC_DIR/core

DEFINES += NO_IRISNET
HEADERS += $$CSRC_DIR/processquit.h
SOURCES += $$CSRC_DIR/processquit.cpp

HEADERS += \
	$$CSRC_DIR/tnetstring.h \
	$$CSRC_DIR/httpheaders.h \
	$$CSRC_DIR/zhttprequestpacket.h \
	$$CSRC_DIR/zhttpresponsepacket.h \
	$$CSRC_DIR/log.h \
	$$CSRC_DIR/bufferlist.h \
	$$CSRC_DIR/layertracker.h

SOURCES += \
	$$CSRC_DIR/tnetstring.cpp \
	$$CSRC_DIR/httpheaders.cpp \
	$$CSRC_DIR/zhttprequestpacket.cpp \
	$$CSRC_DIR/zhttpresponsepacket.cpp \
	$$CSRC_DIR/log.cpp \
	$$CSRC_DIR/bufferlist.cpp \
	$$CSRC_DIR/layertracker.cpp

HEADERS += \
	$$CSRC_DIR/packet/httprequestdata.h \
	$$CSRC_DIR/packet/httpresponsedata.h \
	$$CSRC_DIR/packet/retryrequestpacket.h \
	$$CSRC_DIR/packet/wscontrolpacket.h \
	$$CSRC_DIR/packet/statspacket.h \
	$$CSRC_DIR/packet/zrpcrequestpacket.h \
	$$CSRC_DIR/packet/zrpcresponsepacket.h

SOURCES += \
	$$CSRC_DIR/packet/retryrequestpacket.cpp \
	$$CSRC_DIR/packet/wscontrolpacket.cpp \
	$$CSRC_DIR/packet/statspacket.cpp \
	$$CSRC_DIR/packet/zrpcrequestpacket.cpp \
	$$CSRC_DIR/packet/zrpcresponsepacket.cpp

HEADERS += \
	$$CSRC_DIR/callback.h \
	$$CSRC_DIR/config.h \
	$$CSRC_DIR/timerwheel.h \
	$$CSRC_DIR/jwt.h \
	$$CSRC_DIR/rtimer.h \
	$$CSRC_DIR/logutil.h \
	$$CSRC_DIR/uuidutil.h \
	$$CSRC_DIR/zutil.h \
	$$CSRC_DIR/httprequest.h \
	$$CSRC_DIR/websocket.h \
	$$CSRC_DIR/zhttpmanager.h \
	$$CSRC_DIR/zhttprequest.h \
	$$CSRC_DIR/zwebsocket.h \
	$$CSRC_DIR/zrpcmanager.h \
	$$CSRC_DIR/zrpcrequest.h \
	$$CSRC_DIR/statusreasons.h \
	$$CSRC_DIR/inspectdata.h \
	$$CSRC_DIR/cors.h \
	$$CSRC_DIR/simplehttpserver.h \
	$$CSRC_DIR/stats.h \
	$$CSRC_DIR/statsmanager.h \
	$$CSRC_DIR/settings.h

SOURCES += \
	$$CSRC_DIR/config.cpp \
	$$CSRC_DIR/timerwheel.cpp \
	$$CSRC_DIR/jwt.cpp \
	$$CSRC_DIR/rtimer.cpp \
	$$CSRC_DIR/logutil.cpp \
	$$CSRC_DIR/uuidutil.cpp \
	$$CSRC_DIR/zutil.cpp \
	$$CSRC_DIR/zhttpmanager.cpp \
	$$CSRC_DIR/zhttprequest.cpp \
	$$CSRC_DIR/zwebsocket.cpp \
	$$CSRC_DIR/zrpcmanager.cpp \
	$$CSRC_DIR/zrpcrequest.cpp \
	$$CSRC_DIR/statusreasons.cpp \
	$$CSRC_DIR/cors.cpp \
	$$CSRC_DIR/simplehttpserver.cpp \
	$$CSRC_DIR/stats.cpp \
	$$CSRC_DIR/statsmanager.cpp \
	$$CSRC_DIR/settings.cpp

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

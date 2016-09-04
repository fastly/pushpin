SRC_DIR = $$PWD/..
CORE_DIR = $$PWD/../../corelib
QZMQ_DIR = $$CORE_DIR/qzmq
COMMON_DIR = $$CORE_DIR/common

INCLUDEPATH += $$SRC_DIR
INCLUDEPATH += $$CORE_DIR

INCLUDEPATH += $$QZMQ_DIR/src

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

HEADERS += \
	$$SRC_DIR/jwt.h \
	$$SRC_DIR/testhttprequest.h \
	$$SRC_DIR/testwebsocket.h \
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
	$$SRC_DIR/routesfile.h \
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
	$$SRC_DIR/testhttprequest.cpp \
	$$SRC_DIR/testwebsocket.cpp \
	$$SRC_DIR/websocketoverhttp.cpp \
	$$SRC_DIR/zrpcchecker.cpp \
	$$SRC_DIR/sockjsmanager.cpp \
	$$SRC_DIR/sockjssession.cpp \
	$$SRC_DIR/inspectrequest.cpp \
	$$SRC_DIR/acceptrequest.cpp \
	$$SRC_DIR/connectionmanager.cpp \
	$$SRC_DIR/wscontrolmanager.cpp \
	$$SRC_DIR/wscontrolsession.cpp \
	$$SRC_DIR/routesfile.cpp \
	$$SRC_DIR/domainmap.cpp \
	$$SRC_DIR/zroutes.cpp \
	$$SRC_DIR/requestsession.cpp \
	$$SRC_DIR/proxyutil.cpp \
	$$SRC_DIR/proxysession.cpp \
	$$SRC_DIR/wsproxysession.cpp \
	$$SRC_DIR/updater.cpp \
	$$SRC_DIR/engine.cpp

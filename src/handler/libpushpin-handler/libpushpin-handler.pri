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
	$$SRC_DIR/deferred.h \
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
	$$SRC_DIR/format.h \
	$$SRC_DIR/idformat.h \
	$$SRC_DIR/httpsession.h \
	$$SRC_DIR/httpsessionupdatemanager.h \
	$$SRC_DIR/wssession.h \
	$$SRC_DIR/publishlastids.h \
	$$SRC_DIR/controlrequest.h \
	$$SRC_DIR/conncheckworker.h \
	$$SRC_DIR/refreshworker.h \
	$$SRC_DIR/ratelimiter.h \
	$$SRC_DIR/sequencer.h \
	$$SRC_DIR/filter.h \
	$$SRC_DIR/filterstack.h \
	$$SRC_DIR/engine.h

SOURCES += \
	$$SRC_DIR/deferred.cpp \
	$$SRC_DIR/variantutil.cpp \
	$$SRC_DIR/jsonpointer.cpp \
	$$SRC_DIR/jsonpatch.cpp \
	$$SRC_DIR/sessionrequest.cpp \
	$$SRC_DIR/requeststate.cpp \
	$$SRC_DIR/wscontrolmessage.cpp \
	$$SRC_DIR/publishformat.cpp \
	$$SRC_DIR/publishitem.cpp \
	$$SRC_DIR/instruct.cpp \
	$$SRC_DIR/format.cpp \
	$$SRC_DIR/idformat.cpp \
	$$SRC_DIR/httpsession.cpp \
	$$SRC_DIR/httpsessionupdatemanager.cpp \
	$$SRC_DIR/wssession.cpp \
	$$SRC_DIR/publishlastids.cpp \
	$$SRC_DIR/controlrequest.cpp \
	$$SRC_DIR/conncheckworker.cpp \
	$$SRC_DIR/refreshworker.cpp \
	$$SRC_DIR/ratelimiter.cpp \
	$$SRC_DIR/sequencer.cpp \
	$$SRC_DIR/filter.cpp \
	$$SRC_DIR/filterstack.cpp \
	$$SRC_DIR/engine.cpp

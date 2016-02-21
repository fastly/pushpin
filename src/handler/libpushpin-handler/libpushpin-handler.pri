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

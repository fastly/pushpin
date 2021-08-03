TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib
QT -= gui
QT += network
TARGET = pushpin-handler
DESTDIR = ..

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/../../corelib -lpushpin-core
PRE_TARGETDEPS += $$PWD/../../corelib/libpushpin-core.a

CONFIG(release) {
	LIBS += -L$$PWD/../../../target/release -lpushpin
} else {
	LIBS += -L$$PWD/../../../target/debug -lpushpin
}

include($$OUT_PWD/../../../conf.pri)
include(libpushpin-handler.pri)

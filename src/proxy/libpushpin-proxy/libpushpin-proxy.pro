TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib
QT -= gui
QT += network
TARGET = pushpin-proxy
DESTDIR = ..

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/../../corelib -lpushpin-core
PRE_TARGETDEPS += $$PWD/../../corelib/libpushpin-core.a

include($$OUT_PWD/../../../conf.pri)

CONFIG(debug, debug|release) {
	LIBS += -L$$PWD/../../../target/debug -lpushpin -ldl
	PRE_TARGETDEPS += $$PWD/../../../target/debug/libpushpin.a
} else {
	LIBS += -L$$PWD/../../../target/release -lpushpin -ldl
	PRE_TARGETDEPS += $$PWD/../../../target/release/libpushpin.a
}

include(libpushpin-proxy.pri)

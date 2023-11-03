CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = pushpin-proxy
DESTDIR = ../../../bin

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/.. -lpushpin-proxy
PRE_TARGETDEPS += $$PWD/../libpushpin-proxy.a

LIBS += -L$$PWD/../../cpp -lpushpin-cpp
PRE_TARGETDEPS += $$PWD/../../cpp/libpushpin-cpp.a

include($$OUT_PWD/../../rust/lib.pri)
include($$OUT_PWD/../../../conf.pri)
include(pushpin-proxy.pri)

unix:!isEmpty(BINDIR) {
	target.path = $$BINDIR
	INSTALLS += target
}

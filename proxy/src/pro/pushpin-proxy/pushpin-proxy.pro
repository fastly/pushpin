CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = pushpin-proxy
DESTDIR = ../../..

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/../.. -lpushpin-proxy
PRE_TARGETDEPS += $$PWD/../../libpushpin-proxy.a

include($$OUT_PWD/../../../../conf.pri)
include(pushpin-proxy.pri)

unix:!isEmpty(BINDIR) {
	target.path = $$BINDIR
	INSTALLS += target
}

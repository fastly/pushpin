CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = pushpin-legacy
DESTDIR = ../../..

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/.. -lrunner
PRE_TARGETDEPS += $$PWD/../librunner.a

LIBS += -L$$PWD/../../cpp -lpushpin-cpp
PRE_TARGETDEPS += $$PWD/../../cpp/libpushpin-cpp.a

include($$OUT_PWD/../../../conf.pri)
include(runner.pri)

unix:!isEmpty(BINDIR) {
	target.path = $$BINDIR
	INSTALLS += target
}

libfiles.path = $$LIBDIR/runner
libfiles.files = $$PWD/../*.template

configfiles.path = $$CONFIGDIR/runner
configfiles.files = $$PWD/../../../examples/config/runner/certs

INSTALLS += libfiles configfiles

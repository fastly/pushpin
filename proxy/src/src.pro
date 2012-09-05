CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = pushpin-proxy
DESTDIR = ..

include($$OUT_PWD/../conf.pri)
include(src.pri)

unix:!isEmpty(BINDIR) {
	target.path = $$BINDIR
	INSTALLS += target
}

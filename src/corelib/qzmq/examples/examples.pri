exists($$PWD/../conf.pri):include($$PWD/../conf.pri)

QT -= gui
QT += network

INCLUDEPATH += $$PWD/../src
include($$PWD/../src/src.pri)

TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib c++11
QT -= gui
QT += network
TARGET = pushpin-cpp
DESTDIR = ../../../target/cpp

cpp_build_dir = $$OUT_PWD/../../../target/cpp

MOC_DIR = $$cpp_build_dir/moc
OBJECTS_DIR = $$cpp_build_dir/obj

include($$OUT_PWD/../../../target/cpp/conf.pri)
include(cpp.pri)

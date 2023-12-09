TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib c++11
QT -= gui
QT += network
TARGET = pushpin-cpp

cpp_build_dir = $$OUT_PWD

MOC_DIR = $$cpp_build_dir/moc
OBJECTS_DIR = $$cpp_build_dir/obj

include($$cpp_build_dir/conf.pri)
include(cpp.pri)

TEMPLATE = subdirs

include($$OUT_PWD/../conf.pri)

cpp.subdir = cpp

rust.subdir = rust
rust.depends = cpp

cpp_tests.subdir = cpp/tests
cpp_tests.depends = cpp rust
cpp_tests.CONFIG += no_default_install

pushpin.subdir = pushpin

SUBDIRS += \
	cpp \
	rust \
	cpp_tests \
	pushpin

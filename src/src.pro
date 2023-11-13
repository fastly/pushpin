TEMPLATE = subdirs

include($$OUT_PWD/../conf.pri)

cpp.subdir = cpp

rust.subdir = rust
rust.depends = cpp

pushpin.subdir = pushpin

SUBDIRS += \
	cpp \
	rust \
	pushpin

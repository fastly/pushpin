TEMPLATE = subdirs

cpp.subdir = cpp
tests.subdir = tests
tests.depends = cpp

tests.CONFIG += no_default_install

SUBDIRS += \
	cpp \
	tests

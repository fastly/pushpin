TEMPLATE = subdirs

corelib.subdir = corelib
tests.subdir = tests
tests.depends = corelib

tests.CONFIG += no_default_install

SUBDIRS += \
	corelib \
	tests

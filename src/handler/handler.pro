TEMPLATE = subdirs

libpushpin_handler.subdir = libpushpin-handler
pushpin_handler.subdir = pushpin-handler
pushpin_handler.depends = libpushpin_handler
tests.subdir = tests
tests.depends = libpushpin_handler

tests.CONFIG += no_default_install

SUBDIRS += \
	libpushpin_handler \
	pushpin_handler \
	tests

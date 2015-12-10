TEMPLATE = subdirs

sub_libpushpin_handler.subdir = src/pro/libpushpin-handler
sub_pushpin_handler.subdir = src/pro/pushpin-handler
sub_pushpin_handler.depends = sub_libpushpin_handler
sub_tests.subdir = tests
sub_tests.depends = sub_libpushpin_handler

SUBDIRS += \
	sub_libpushpin_handler \
	sub_pushpin_handler \
	sub_tests

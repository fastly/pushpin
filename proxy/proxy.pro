TEMPLATE = subdirs

sub_libpushpin_proxy.subdir = src/pro/libpushpin-proxy
sub_pushpin_proxy.subdir = src/pro/pushpin-proxy
sub_pushpin_proxy.depends = sub_libpushpin_proxy
#sub_tests.subdir = tests
#sub_tests.depends = sub_libpushpin_proxy

SUBDIRS += \
	sub_libpushpin_proxy \
	sub_pushpin_proxy

TEMPLATE = subdirs

libpushpin_proxy.subdir = libpushpin-proxy
pushpin_proxy.subdir = pushpin-proxy
pushpin_proxy.depends = libpushpin_proxy
tests.subdir = tests
tests.depends = libpushpin_proxy

tests.CONFIG += no_default_install

SUBDIRS += \
	libpushpin_proxy \
	pushpin_proxy \
	tests

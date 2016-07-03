TEMPLATE = subdirs

librunner.subdir = librunner
runner.subdir = runner
runner.depends = librunner
tests.subdir = tests
tests.depends = librunner

tests.CONFIG += no_default_install

SUBDIRS += \
	librunner \
	runner \
	tests

TEMPLATE = subdirs

cpp.subdir = cpp

tests.subdir = tests
tests.depends = cpp

SUBDIRS += \
	cpp \
	tests

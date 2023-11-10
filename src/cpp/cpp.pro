TEMPLATE = subdirs

makedirs.subdir = makedirs

cpp.subdir = cpp
cpp.depends = makedirs

tests.subdir = tests
tests.depends = cpp makedirs

SUBDIRS += \
	makedirs \
	cpp \
	tests

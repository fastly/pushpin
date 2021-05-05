TEMPLATE = subdirs

include($$OUT_PWD/../conf.pri)

corelib.subdir = corelib

m2adapter.subdir = m2adapter
m2adapter.depends = corelib

proxy.subdir = proxy
proxy.depends = corelib

handler.subdir = handler
handler.depends = corelib

runner.subdir = runner
runner.depends = corelib

rust.subdir = rust

pushpin.subdir = pushpin

SUBDIRS += \
	corelib \
	m2adapter \
	proxy \
	handler \

CONFIG(use_cargo) {
	SUBDIRS += rust
}

SUBDIRS += \
	runner \
	pushpin

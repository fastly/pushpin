TEMPLATE = subdirs

corelib.subdir = corelib

m2adapter.subdir = m2adapter
m2adapter.depends = corelib

proxy.subdir = proxy
proxy.depends = corelib

handler.subdir = handler
handler.depends = corelib

runner.subdir = runner
runner.depends = corelib

pushpin.subdir = pushpin

SUBDIRS += \
	corelib \
	m2adapter \
	proxy \
	handler \
	runner \
	pushpin

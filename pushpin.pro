TEMPLATE = subdirs

m2adapter.subdir = m2adapter
proxy.subdir = proxy
handler.subdir = handler
tools.subdir = tools/publish

SUBDIRS += \
	m2adapter \
	proxy \
	handler \
	tools

include($$OUT_PWD/conf.pri)

runnerlibfiles.path = $$LIBDIR/runner
runnerlibfiles.files = runner/processmanager.py runner/services.py runner/runner.py

runnerconfigfiles.path = $$CONFIGDIR/runner
runnerconfigfiles.files = runner/mongrel2.conf.template runner/m2adapter.conf.template runner/zurl.conf.template runner/certs

runnerbinfiles.path = $$BINDIR
runnerbinfiles.extra = cp -f pushpin.inst $(INSTALL_ROOT)$$runnerbinfiles.path/pushpin

pushpinconfigfiles.path = $$CONFIGDIR
pushpinconfigfiles.files = examples/config/internal.conf

routes.path = $$CONFIGDIR
routes.extra = test -e $(INSTALL_ROOT)$$routes.path/routes || cp -f examples/config/routes $(INSTALL_ROOT)$$routes.path/routes

pushpinconf.path = $$CONFIGDIR
pushpinconf.extra = test -e $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf || cp -f pushpin.conf.inst $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf

INSTALLS += runnerlibfiles runnerconfigfiles runnerbinfiles pushpinconfigfiles routes pushpinconf

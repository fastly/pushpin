TEMPLATE = subdirs

src.subdir = src
tools.subdir = tools

SUBDIRS += src tools

include($$OUT_PWD/conf.pri)

runnerlibfiles.path = $$LIBDIR/runner
runnerlibfiles.files = src/runner/processmanager.py src/runner/services.py src/runner/runner.py

runnerconfigfiles.path = $$CONFIGDIR/runner
runnerconfigfiles.files = src/runner/mongrel2.conf.template src/runner/m2adapter.conf.template src/runner/zurl.conf.template src/runner/certs

runnerbinfiles.path = $$BINDIR
runnerbinfiles.extra = cp -f pushpin.inst $(INSTALL_ROOT)$$runnerbinfiles.path/pushpin

pushpinconfigfiles.path = $$CONFIGDIR
pushpinconfigfiles.files = examples/config/internal.conf

routes.path = $$CONFIGDIR
routes.extra = test -e $(INSTALL_ROOT)$$routes.path/routes || cp -f examples/config/routes $(INSTALL_ROOT)$$routes.path/routes

pushpinconf.path = $$CONFIGDIR
pushpinconf.extra = test -e $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf || cp -f pushpin.conf.inst $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf

INSTALLS += runnerlibfiles runnerconfigfiles runnerbinfiles pushpinconfigfiles routes pushpinconf

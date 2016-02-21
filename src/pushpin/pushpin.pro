TEMPLATE = aux

include($$OUT_PWD/../../conf.pri)

# generate pushpin.conf for installation

pushpin_conf_inst.target = pushpin.conf.inst
pushpin_conf_inst.commands = sed -e \"s,configdir=.*,configdir=$$CONFIGDIR/runner,g\" -e \"s,rundir=.*,rundir=$$RUNDIR,g\" -e \"s,logdir=.*,logdir=$$LOGDIR,g\" ../../examples/config/pushpin.conf > pushpin.conf.inst
pushpin_conf_inst.depends = ../../examples/config/pushpin.conf

QMAKE_EXTRA_TARGETS += pushpin_conf_inst
PRE_TARGETDEPS += pushpin.conf.inst

# generate pushpin launcher for installation
# FIXME: if runner becomes a qmake project, move this

pushpin_inst.target = pushpin.inst
pushpin_inst.commands = sed -e \"s,^default_libdir = .*,default_libdir = \'$$LIBDIR\',g\" ../../pushpin | sed -e \"s,^default_configdir =.*,default_configdir = \'$$CONFIGDIR\',g\" | sed -e \"s,^version =.*,version = \'$$APP_VERSION\',g\" > pushpin.inst && chmod 755 pushpin.inst
pushpin_inst.depends = ../../pushpin

QMAKE_EXTRA_TARGETS += pushpin_inst
PRE_TARGETDEPS += pushpin.inst

# install runner files
# FIXME: if runner becomes a qmake project, move this

runnerlibfiles.path = $$LIBDIR/runner
runnerlibfiles.files = ../runner/*.py ../runner/*.template

runnerconfigfiles.path = $$CONFIGDIR/runner
runnerconfigfiles.files = ../runner/certs

runnerbinfiles.path = $$BINDIR
runnerbinfiles.extra = cp -f pushpin.inst $(INSTALL_ROOT)$$runnerbinfiles.path/pushpin

INSTALLS += runnerlibfiles runnerconfigfiles runnerbinfiles

# install config files

pushpinconfigfiles.path = $$CONFIGDIR
pushpinconfigfiles.files = ../../examples/config/internal.conf

routes.path = $$CONFIGDIR
routes.extra = test -e $(INSTALL_ROOT)$$routes.path/routes || cp -f ../../examples/config/routes $(INSTALL_ROOT)$$routes.path/routes

pushpinconf.path = $$CONFIGDIR
pushpinconf.extra = test -e $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf || cp -f pushpin.conf.inst $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf

INSTALLS += pushpinconfigfiles routes pushpinconf

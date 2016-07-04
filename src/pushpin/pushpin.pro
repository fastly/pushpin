TEMPLATE = aux

include($$OUT_PWD/../../conf.pri)

# generate pushpin.conf for installation

pushpin_conf_inst.target = pushpin.conf.inst
pushpin_conf_inst.commands = sed -e \"s,configdir=.*,configdir=$$CONFIGDIR/runner,g\" -e \"s,rundir=.*,rundir=$$RUNDIR,g\" -e \"s,logdir=.*,logdir=$$LOGDIR,g\" ../../examples/config/pushpin.conf > pushpin.conf.inst
pushpin_conf_inst.depends = ../../examples/config/pushpin.conf

QMAKE_EXTRA_TARGETS += pushpin_conf_inst
PRE_TARGETDEPS += pushpin.conf.inst

# install general lib files

libfiles.path = $$LIBDIR
libfiles.files = internal.conf

# install config files

routes.path = $$CONFIGDIR
routes.extra = test -e $(INSTALL_ROOT)$$routes.path/routes || cp -f ../../examples/config/routes $(INSTALL_ROOT)$$routes.path/routes

pushpinconf.path = $$CONFIGDIR
pushpinconf.extra = test -e $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf || cp -f pushpin.conf.inst $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf

INSTALLS += libfiles routes pushpinconf

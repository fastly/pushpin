TEMPLATE = aux

include($$OUT_PWD/../conf.pri)

root_dir = $$PWD/..
bin_dir = $$root_dir/bin

# generate pushpin.conf for installation

pushpin_conf_inst.target = pushpin.conf.inst
pushpin_conf_inst.commands = sed -e \"s,configdir=.*,configdir=$$CONFIGDIR/runner,g\" -e \"s,rundir=.*,rundir=$$RUNDIR,g\" -e \"s,logdir=.*,logdir=$$LOGDIR,g\" ../examples/config/pushpin.conf > pushpin.conf.inst
pushpin_conf_inst.depends = ../examples/config/pushpin.conf

QMAKE_EXTRA_TARGETS += pushpin_conf_inst
PRE_TARGETDEPS += pushpin.conf.inst

# install bin files

unix:!isEmpty(BINDIR) {
	binfiles.path = $$BINDIR
	binfiles.files = \
		$$bin_dir/condure \
		$$bin_dir/m2adapter \
		$$bin_dir/pushpin-proxy \
		$$bin_dir/pushpin-handler \
		$$root_dir/pushpin-legacy \
		$$root_dir/pushpin \
		$$bin_dir/pushpin-publish
	binfiles.CONFIG += no_check_exist executable

	INSTALLS += binfiles
}

# install lib files

libfiles.path = $$LIBDIR
libfiles.files = $$PWD/../src/internal.conf

runnerlibfiles.path = $$LIBDIR/runner
runnerlibfiles.files = $$PWD/../src/runner/*.template

# install config files

runnerconfigfiles.path = $$CONFIGDIR/runner
runnerconfigfiles.files = $$PWD/../examples/config/runner/certs

routes.path = $$CONFIGDIR
routes.extra = test -e $(INSTALL_ROOT)$$routes.path/routes || cp -f ../examples/config/routes $(INSTALL_ROOT)$$routes.path/routes

pushpinconf.path = $$CONFIGDIR
pushpinconf.extra = test -e $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf || cp -f pushpin.conf.inst $(INSTALL_ROOT)$$pushpinconf.path/pushpin.conf

INSTALLS += libfiles runnerlibfiles runnerconfigfiles routes pushpinconf

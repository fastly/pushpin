TEMPLATE = aux
CONFIG -= debug_and_release debug

include($$OUT_PWD/../target/postbuild_conf.pri)

root_dir = $$PWD/..
bin_dir = $$root_dir/bin

CONFIG(debug, debug|release) {
	cargo_flags =
	target_dir = $$root_dir/target/debug
} else {
	cargo_flags = --release
	target_dir = $$root_dir/target/release
}

# copy bin files

condure_bin.target = $$bin_dir/condure
condure_bin.depends = $$target_dir/condure
condure_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/condure $$bin_dir/condure

m2adapter_bin.target = $$bin_dir/m2adapter
m2adapter_bin.depends = $$target_dir/m2adapter
m2adapter_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/m2adapter $$bin_dir/m2adapter

proxy_bin.target = $$bin_dir/pushpin-proxy
proxy_bin.depends = $$target_dir/pushpin-proxy
proxy_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-proxy $$bin_dir/pushpin-proxy

handler_bin.target = $$bin_dir/pushpin-handler
handler_bin.depends = $$target_dir/pushpin-handler
handler_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-handler $$bin_dir/pushpin-handler

runner_legacy_bin.target = $$root_dir/pushpin-legacy
runner_legacy_bin.depends = $$target_dir/pushpin-legacy
runner_legacy_bin.commands = cp -a $$target_dir/pushpin-legacy $$root_dir/pushpin-legacy

runner_bin.target = $$root_dir/pushpin
runner_bin.depends = $$target_dir/pushpin
runner_bin.commands = cp -a $$target_dir/pushpin $$root_dir/pushpin

publish_bin.target = $$bin_dir/pushpin-publish
publish_bin.depends = $$target_dir/pushpin-publish
publish_bin.commands = mkdir -p $$bin_dir && cp -a $$target_dir/pushpin-publish $$bin_dir/pushpin-publish

QMAKE_EXTRA_TARGETS += \
	condure_bin \
	m2adapter_bin \
	proxy_bin \
	handler_bin \
	runner_legacy_bin \
	runner_bin \
	publish_bin

PRE_TARGETDEPS += \
	$$bin_dir/condure \
	$$bin_dir/m2adapter \
	$$bin_dir/pushpin-proxy \
	$$bin_dir/pushpin-handler \
	$$root_dir/pushpin-legacy \
	$$root_dir/pushpin \
	$$bin_dir/pushpin-publish

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

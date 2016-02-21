TEMPLATE = aux

include($$OUT_PWD/../../conf.pri)

pushpin_conf_inst.target = ../../pushpin.conf.inst
pushpin_conf_inst.commands = sed -e \"s,configdir=.*,configdir=$$CONFIGDIR/runner,g\" -e \"s,rundir=.*,rundir=$$RUNDIR,g\" -e \"s,logdir=.*,logdir=$$LOGDIR,g\" ../../examples/config/pushpin.conf > ../../pushpin.conf.inst
pushpin_conf_inst.depends = ../../examples/config/pushpin.conf

pushpin_inst.target = ../../pushpin.inst
pushpin_inst.commands = sed -e \"s,^default_libdir = .*,default_libdir = \'$$LIBDIR\',g\" ../../pushpin | sed -e \"s,^default_configdir =.*,default_configdir = \'$$CONFIGDIR\',g\" | sed -e \"s,^version =.*,version = \'$$APP_VERSION\',g\" > ../../pushpin.inst && chmod 755 ../../pushpin.inst
pushpin_inst.depends = ../../pushpin

QMAKE_EXTRA_TARGETS += pushpin_conf_inst pushpin_inst

PRE_TARGETDEPS += ../../pushpin.conf.inst ../../pushpin.inst

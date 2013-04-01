import sys
import os
import ConfigParser
from processmanager import ProcessManager
import services

def run(exedir, config_file, verbose):
	config = ConfigParser.ConfigParser()
	config.read([config_file])

	configdir = config.get("runner", "configdir")
	if not os.path.isabs(configdir):
		configdir = os.path.join(os.path.dirname(config_file), configdir)

	service_names = config.get("runner", "services").split(",")

	http_port = int(config.get("runner", "http_port"))

	rundir = config.get("runner", "rundir")
	if not os.path.isabs(rundir):
		rundir = os.path.join(os.path.dirname(config_file), rundir)

	logdir = config.get("runner", "logdir")
	if not os.path.isabs(logdir):
		logdir = os.path.join(os.path.dirname(config_file), logdir)

	proxybin = "pushpin-proxy"
	path = os.path.normpath(os.path.join(exedir, "proxy/pushpin-proxy"))
	if os.path.isfile(path):
		proxybin = path

	handlerbin = "pushpin-handler"
	path = os.path.normpath(os.path.join(exedir, "handler/pushpin-handler"))
	if os.path.isfile(path):
		handlerbin = path

	service_objs = list()
	for name in service_names:
		if name == "mongrel2":
			service_objs.append(services.Mongrel2Service("mongrel2", os.path.join(configdir, "mongrel2.conf.template"), http_port, configdir, rundir, logdir))
		elif name == "zurl":
			service_objs.append(services.ZurlService("zurl", os.path.join(configdir, "zurl.conf"), verbose, rundir, logdir))
		elif name == "pushpin-proxy":
			service_objs.append(services.PushpinProxyService(proxybin, config_file, verbose, rundir, logdir))
		elif name == "pushpin-handler":
			service_objs.append(services.PushpinHandlerService(handlerbin, config_file, rundir, logdir))

	print "starting..."

	p = ProcessManager()
	p.stopmessage = "stopping..."

	for s in service_objs:
		s.pre_start()
		pid = p.add(s.name(), s.getargs(), s.getlogfile())
		s.post_start(pid)

	print "started"

	p.wait()

	for s in service_objs:
		s.post_stop()

	print "stopped"

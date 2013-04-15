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

	https_ports = list()
	for p in config.get("runner", "https_ports").split(","):
		if not p:
			continue
		https_ports.append(int(p))

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

	if "mongrel2" in service_names:
		mongrel2_bin = "mongrel2"
		if config.has_option("runner", "mongrel2_bin"):
			mongrel2_bin = config.get("runner", "mongrel2_bin")
		m2sh_bin = "m2sh"
		if config.has_option("runner", "m2sh_bin"):
			m2sh_bin = config.get("runner", "m2sh_bin")

		m2sqlpath = services.write_mongrel2_config(configdir, os.path.join(configdir, "mongrel2.conf.template"), rundir, http_port, https_ports, m2sh_bin)

		service_objs.append(services.Mongrel2Service(mongrel2_bin, m2sqlpath, False, http_port, rundir, logdir))
		for port in https_ports:
			service_objs.append(services.Mongrel2Service(mongrel2_bin, m2sqlpath, True, port, rundir, logdir))

	if "zurl" in service_names:
		zurl_bin = "zurl"
		if config.has_option("runner", "zurl_bin"):
			zurl_bin = config.get("runner", "zurl_bin")
		service_objs.append(services.ZurlService(zurl_bin, os.path.join(configdir, "zurl.conf"), verbose, rundir, logdir))

	if "pushpin-proxy" in service_names:
		service_objs.append(services.PushpinProxyService(proxybin, config_file, verbose, rundir, logdir))

	if "pushpin-handler" in service_names:
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

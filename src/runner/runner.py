import os
import errno
import ConfigParser
from processmanager import ProcessManager
import services

def trymakedir(dir):
	try:
		os.makedirs(dir)
	except OSError as e:
		if e.errno != errno.EEXIST:
			raise

def parse_port(s):
	at = s.find(":")
	if at != -1:
		return (s[:at], int(s[at + 1:]))
	else:
		return ("0.0.0.0", int(s))

def run(exedir, libdir, configdir, config_file, verbose):
	config = ConfigParser.ConfigParser()
	config.read([config_file])

	service_names = config.get("runner", "services").split(",")

	http_port = parse_port(config.get("runner", "http_port"))

	https_ports = list()
	if config.has_option("runner", "https_ports"):
		for p in config.get("runner", "https_ports").split(","):
			if not p:
				continue
			https_ports.append(parse_port(p))

	if config.has_option("global", "rundir"):
		rundir = config.get("global", "rundir")
	else:
		print 'warning: rundir in [runner] section is deprecated. put in [global]'
		rundir = config.get("runner", "rundir")
	if not os.path.isabs(rundir):
		rundir = os.path.abspath(rundir)

	logdir = config.get("runner", "logdir")
	if not os.path.isabs(logdir):
		logdir = os.path.abspath(logdir)

	m2abin = "m2adapter"
	path = os.path.normpath(os.path.join(exedir, "bin/m2adapter"))
	if os.path.isfile(path):
		m2abin = path

	proxybin = "pushpin-proxy"
	path = os.path.normpath(os.path.join(exedir, "bin/pushpin-proxy"))
	if os.path.isfile(path):
		proxybin = path

	handlerbin = "pushpin-handler"
	path = os.path.normpath(os.path.join(exedir, "bin/pushpin-handler"))
	if os.path.isfile(path):
		handlerbin = path

	# make run/log dirs if needed. don't fail if dirs already exist
	trymakedir(rundir)
	trymakedir(logdir)

	service_objs = list()

	if "mongrel2" in service_names:
		mongrel2_bin = "mongrel2"
		if config.has_option("runner", "mongrel2_bin"):
			mongrel2_bin = config.get("runner", "mongrel2_bin")
		m2sh_bin = "m2sh"
		if config.has_option("runner", "m2sh_bin"):
			m2sh_bin = config.get("runner", "m2sh_bin")

		m2sqlpath = services.write_mongrel2_config(configdir, os.path.join(libdir, "mongrel2.conf.template"), rundir, logdir, http_port, https_ports, m2sh_bin)

		service_objs.append(services.Mongrel2Service(mongrel2_bin, m2sqlpath, False, http_port[1], rundir, logdir))
		for port in https_ports:
			service_objs.append(services.Mongrel2Service(mongrel2_bin, m2sqlpath, True, port[1], rundir, logdir))

	if "m2adapter" in service_names:
		ports = list()
		ports.append(http_port[1])
		ports.extend([port[1] for port in https_ports])
		services.write_m2adapter_config(os.path.join(libdir, "m2adapter.conf.template"), rundir, ports)
		service_objs.append(services.M2AdapterService(m2abin, os.path.join(rundir, "m2adapter.conf"), verbose, rundir, logdir))

	if "zurl" in service_names:
		services.write_zurl_config(os.path.join(libdir, "zurl.conf.template"), rundir)
		zurl_bin = "zurl"
		if config.has_option("runner", "zurl_bin"):
			zurl_bin = config.get("runner", "zurl_bin")
		service_objs.append(services.ZurlService(zurl_bin, os.path.join(rundir, "zurl.conf"), verbose, rundir, logdir))

	if "pushpin-proxy" in service_names:
		service_objs.append(services.PushpinProxyService(proxybin, config_file, verbose, rundir, logdir))

	if "pushpin-handler" in service_names:
		service_objs.append(services.PushpinHandlerService(handlerbin, config_file, verbose, rundir, logdir))

	print "starting..."

	p = ProcessManager()
	p.reloadmessage = "reloading"
	p.stopmessage = "stopping..."

	for s in service_objs:
		s.pre_start()
		pid = p.add(s.name(), s.getargs(), s.getlogfile(), s.accept_sighup())
		s.post_start(pid)

	print "started"

	p.wait()

	for s in service_objs:
		s.post_stop()

	print "stopped"

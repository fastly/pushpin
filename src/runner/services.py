import os
import subprocess
import jinja2

def compile_template(infilename, outfilename, vars):
	e = jinja2.Environment()
	f = open(infilename, "r")
	t = e.from_string(f.read())
	f.close()
	out = t.render(vars) + "\n"
	f = open(outfilename, "w")
	f.write(out)
	f.close()

# return path of sql config
def write_mongrel2_config(rootdir, configpath, rundir, logdir, http_port, https_ports, shbinpath):
	assert(configpath.endswith(".template"))
	fname = os.path.basename(configpath)
	path, ext = os.path.splitext(fname)
	genconfigpath = os.path.join(rundir, path)

	ports = list()
	ports.append({ "ssl": False, "value": http_port[1], "addr": http_port[0] })
	for p in https_ports:
		ports.append({ "ssl": True, "value": p[1], "addr": p[0] })

	cwd = os.getcwd()

	vars = dict()
	vars["ports"] = ports
	vars["certdir"] = os.path.join(os.path.relpath(rootdir, cwd), "certs")
	vars["rundir"] = os.path.relpath(rundir, cwd)
	vars["logdir"] = os.path.relpath(logdir, cwd)
	compile_template(configpath, genconfigpath, vars)

	path, ext = os.path.splitext(genconfigpath)
	sqlconfigpath = path + ".sqlite"

	# generate sqlite config
	subprocess.check_call([shbinpath, "load", "-config", genconfigpath, "-db", sqlconfigpath])

	return sqlconfigpath

def write_m2adapter_config(configpath, rundir, ports):
	assert(configpath.endswith(".template"))
	fname = os.path.basename(configpath)
	path, ext = os.path.splitext(fname)
	genconfigpath = os.path.join(rundir, path)

	instances = list()
	for port in ports:
		i = dict()
		i["send_spec"] = "ipc://%s/pushpin-m2-out-%d" % (rundir, port)
		i["recv_spec"] = "ipc://%s/pushpin-m2-in-%d" % (rundir, port)
		i["send_ident"] = "pushpin-m2-%d" % port
		i["control_spec"] = "ipc://%s/pushpin-m2-control-%d" % (rundir, port)
		instances.append(i)

	vars = dict()
	vars["instances"] = instances
	vars["rundir"] = rundir
	compile_template(configpath, genconfigpath, vars)

def write_zurl_config(configpath, rundir):
	assert(configpath.endswith(".template"))
	fname = os.path.basename(configpath)
	path, ext = os.path.splitext(fname)
	genconfigpath = os.path.join(rundir, path)

	vars = dict()
	vars["rundir"] = rundir
	compile_template(configpath, genconfigpath, vars)

class Service(object):
	def __init__(self, rundir, logdir):
		self.rundir = rundir
		self.logdir = logdir

	def name(self):
		pass

	def getlogfile(self):
		return os.path.join(self.logdir, self.name() + ".log")

	def getpidfile(self):
		return os.path.join(self.rundir, self.name() + ".pid")

	def getargs(self):
		pass

	def accept_sighup(self):
		return False

	def pre_start(self):
		pass

	def post_start(self, pid):
		pidfilename = self.getpidfile()
		if pidfilename:
			pidfile = open(pidfilename, "w")
			pidfile.write(str(pid) + "\n")
			pidfile.close()

	def post_stop(self):
		pidfilename = self.getpidfile()
		if pidfilename:
			os.remove(pidfilename)

class Mongrel2Service(Service):
	def __init__(self, binpath, sqlconfigpath, ssl, port, rundir, logdir):
		super(Mongrel2Service, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.sqlconfigpath = sqlconfigpath
		self.ssl = ssl
		self.port = port

	def name(self):
		if self.ssl:
			proto = "https"
		else:
			proto = "http"
		return "mongrel2 (%s:%d)" % (proto, self.port)

	def getlogfile(self):
		return os.path.join(self.logdir, "mongrel2_%d.log" % self.port)

	def getpidfile(self):
		# mongrel2 writes its own pid file
		return None

	def getargs(self):
		return [self.binpath, self.sqlconfigpath, "default_%d" % self.port]

	def accept_sighup(self):
		return True

	def pre_start(self):
		super(Mongrel2Service, self).pre_start()

		# mongrel2 will refuse to start if it sees a pidfile
		pidfilename = os.path.join(self.rundir, "mongrel2_%d.pid" % self.port)
		if os.path.isfile(pidfilename):
			os.remove(pidfilename)

class ZurlService(Service):
	def __init__(self, binpath, configpath, verbose, rundir, logdir):
		super(ZurlService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath
		self.verbose = verbose

	def name(self):
		return "zurl"

	def getlogfile(self):
		return None

	def getargs(self):
		args = list()
		args.append(self.binpath)
		if self.verbose:
			args.append("--verbose")
		args.append("--config=%s" % self.configpath)
		args.append("--logfile=%s" % super(ZurlService, self).getlogfile())
		return args

	def accept_sighup(self):
		return True

class M2AdapterService(Service):
	def __init__(self, binpath, configpath, verbose, rundir, logdir):
		super(M2AdapterService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath
		self.verbose = verbose

	def name(self):
		return "m2adapter"

	def getlogfile(self):
		return None

	def getargs(self):
		args = list()
		args.append(self.binpath)
		if self.verbose:
			args.append("--verbose")
		args.append("--config=%s" % self.configpath)
		args.append("--logfile=%s" % super(M2AdapterService, self).getlogfile())
		return args

	def accept_sighup(self):
		return True

class PushpinProxyService(Service):
	def __init__(self, binpath, configpath, verbose, rundir, logdir):
		super(PushpinProxyService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath
		self.verbose = verbose

	def name(self):
		return "pushpin-proxy"

	def getlogfile(self):
		return None

	def getargs(self):
		args = list()
		args.append(self.binpath)
		if self.verbose:
			args.append("--verbose")
		args.append("--config=%s" % self.configpath)
		args.append("--logfile=%s" % super(PushpinProxyService, self).getlogfile())
		return args

	def accept_sighup(self):
		return True

class PushpinHandlerService(Service):
	def __init__(self, binpath, configpath, verbose, rundir, logdir):
		super(PushpinHandlerService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath
		self.verbose = verbose

	def name(self):
		return "pushpin-handler"

	def getlogfile(self):
		return None

	def getargs(self):
		args = list()
		args.append(self.binpath)
		if self.verbose:
			args.append("--verbose")
		args.append("--config=%s" % self.configpath)
		args.append("--logfile=%s" % super(PushpinHandlerService, self).getlogfile())
		return args

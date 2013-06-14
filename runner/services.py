import os
import subprocess
import jinja2

def compile_template(infilename, outfilename, vars):
	e = jinja2.Environment()
	f = open(infilename, "r")
	t = e.from_string(f.read())
	f.close()
	out = t.render(vars)
	f = open(outfilename, "w")
	f.write(out)
	f.close()

# return path of sql config
def write_mongrel2_config(rootdir, configpath, rundir, http_port, https_ports, shbinpath):
	# calculate mongrel2 relative rootdir
	absroot = os.path.abspath(rootdir)
	path = os.path.relpath(absroot, os.getcwd())
	if path.startswith(".."):
		raise ValueError("cannot run from deeper than %s" % absroot)

	if path.startswith("."):
		path = path[1:]
	if len(path) > 0 and not path.startswith("/"):
		path = "/" + path
	rootdir = path

	assert(configpath.endswith(".template"))
	fname = os.path.basename(configpath)
	path, ext = os.path.splitext(fname)
	genconfigpath = os.path.join(rundir, path)

	ports = list()
	ports.append({ "ssl": False, "value": http_port })
	for p in https_ports:
		ports.append({ "ssl": True, "value": p })

	vars = dict()
	vars["ports"] = ports
	vars["rootdir"] = rootdir
	vars["rundir"] = rundir
	vars["rundirabs"] = os.path.abspath(rundir)
	compile_template(configpath, genconfigpath, vars)

	path, ext = os.path.splitext(genconfigpath)
	sqlconfigpath = path + ".sqlite"

	# generate sqlite config
	subprocess.check_call([shbinpath, "load", "-config", genconfigpath, "-db", sqlconfigpath])

	return sqlconfigpath

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
		return [self.binpath, self.sqlconfigpath, "pushpin-m2-%d" % self.port]

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

	def getargs(self):
		args = list()
		args.append(self.binpath)
		if self.verbose:
			args.append("--verbose")
		args.append("--config=%s" % self.configpath)
		return args

class PushpinProxyService(Service):
	def __init__(self, binpath, configpath, verbose, rundir, logdir):
		super(PushpinProxyService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath
		self.verbose = verbose

	def name(self):
		return "pushpin-proxy"

	def getargs(self):
		args = list()
		args.append(self.binpath)
		if self.verbose:
			args.append("--verbose")
		args.append("--config=%s" % self.configpath)
		return args

class PushpinHandlerService(Service):
	def __init__(self, binpath, configpath, rundir, logdir):
		super(PushpinHandlerService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "pushpin-handler"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

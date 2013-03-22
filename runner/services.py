import os
import subprocess

def compile_template(infilename, outfilename, vars):
	f = open(infilename, "r")
	buf = f.read()
	f.close()
	buf = buf.replace("{{ port }}", vars["port"])
	buf = buf.replace("{{ rootdir }}", vars["rootdir"])
	f = open(outfilename, "w")
	f.write(buf)
	f.close()

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
	def __init__(self, binpath, configpath, port, rootdir, rundir, logdir):
		super(Mongrel2Service, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath
		self.port = port
		self.rootdir = rootdir

	def name(self):
		return "mongrel2"

	def getpidfile(self):
		# mongrel2 writes its own pid file
		return None

	def getargs(self):
		return [self.binpath, self.sqlconfigpath, "default"]

	def pre_start(self):
		super(Mongrel2Service, self).pre_start()

		# calculate mongrel2 relative rootdir
		absroot = os.path.abspath(self.rootdir)
		path = os.path.relpath(absroot, os.getcwd())
		if path.startswith(".."):
			raise ValueError("cannot run from deeper than %s" % absroot)

		if path.startswith("."):
			path = path[1:]
		if not path.startswith("/"):
			path = "/" + path
		self.rootdir = path

		assert(self.configpath.endswith(".template"))
		fname = os.path.basename(self.configpath)
		path, ext = os.path.splitext(fname)
		genconfigpath = os.path.join(self.rundir, path)

		vars = dict()
		vars["port"] = str(self.port)
		vars["rootdir"] = self.rootdir
		compile_template(self.configpath, genconfigpath, vars)

		path, ext = os.path.splitext(genconfigpath)
		self.sqlconfigpath = path + ".sqlite"

		# generate sqlite config
		subprocess.check_call(["m2sh", "load", "-config", genconfigpath, "-db", self.sqlconfigpath])

class ZurlService(Service):
	def __init__(self, binpath, configpath, rundir, logdir):
		super(ZurlService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "zurl"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

class PushpinProxyService(Service):
	def __init__(self, binpath, configpath, rundir, logdir):
		super(PushpinProxyService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "pushpin-proxy"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

class PushpinHandlerService(Service):
	def __init__(self, binpath, configpath, rundir, logdir):
		super(PushpinHandlerService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "pushpin-handler"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

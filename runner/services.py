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
	Stopped, Started, Stopping = range(3)

	def __init__(self):
		self.state = Service.Stopped

	def name(self):
		pass

	def start(self):
		pass

	def stop(self):
		pass

	# call every second
	def process(self):
		pass

	def is_stopped(self):
		return self.state == Service.Stopped

class SingleProcessService(Service):
	def __init__(self, rundir, logdir):
		super(SingleProcessService, self).__init__()
		self.rundir = rundir
		self.logdir = logdir
		self.timeleft = None
		self.returncode = None

	def getargs(self):
		pass

	def getlogfile(self):
		return self.name() + ".log"

	def getpidfile(self):
		return self.name() + ".pid"

	def start(self):
		assert(self.state == Service.Stopped)

		try:
			logfile = open(os.path.join(self.logdir, self.getlogfile()), "w")
			self.proc = subprocess.Popen(self.getargs(), stdout=logfile, stderr=subprocess.STDOUT)

			pidfilename = self.getpidfile()
			if pidfilename:
				pidfile = open(os.path.join(self.rundir, self.getpidfile()), "w")
				pidfile.write(str(self.proc.pid) + "\n")
				pidfile.close()

			self.state = Service.Started
			return True
		except Exception as e:
			print e.message
			return False

	def stop(self):
		assert(self.state == Service.Started)

		try:
			self.proc.terminate()
		except:
			pass

		self.state = Service.Stopping
		self.timeleft = 10
		return True

	def process(self):
		assert(self.state == Service.Started or self.state == Service.Stopping)

		try:
			if self.proc.poll() is not None:
				print "retcode: %d" % self.proc.returncode
				if self.state != Service.Stopping:
					print "finished but we didn't ask"
					return False
				self.returncode = self.proc.returncode
				self.proc = None
				self.state = Service.Stopped
			elif self.state == Service.Stopping:
				if self.timeleft <= 0:
					self.proc.kill()
					self.proc = None
					self.state = Service.Stopped
					self.timeleft = None
				else:
					self.timeleft -= 1
			return True
		except BaseException as e:
			print "failed to poll/communicate: %s" % e.__name__
			return False

class Mongrel2Service(SingleProcessService):
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

	def start(self):
		# calculate mongrel2 relative rootdir
		path = os.path.relpath(os.path.abspath(self.rootdir), os.getcwd())
		if path.startswith(".."):
			print "bad relpath: %s" % path
			return False
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
		try:
			subprocess.check_call(["m2sh", "load", "-config", genconfigpath, "-db", self.sqlconfigpath])
		except:
			return False

		return super(Mongrel2Service, self).start()

	def getargs(self):
		return [self.binpath, self.sqlconfigpath, "default"]

class ZurlService(SingleProcessService):
	def __init__(self, binpath, configpath, rundir, logdir):
		super(ZurlService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "zurl"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

class PushpinProxyService(SingleProcessService):
	def __init__(self, binpath, configpath, rundir, logdir):
		super(PushpinProxyService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "pushpin-proxy"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

class PushpinHandlerService(SingleProcessService):
	def __init__(self, binpath, configpath, rundir, logdir):
		super(PushpinHandlerService, self).__init__(rundir, logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "pushpin-handler"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

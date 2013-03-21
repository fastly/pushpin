import os
import subprocess

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
	def __init__(self, logdir):
		super(SingleProcessService, self).__init__()
		self.logdir = logdir
		self.timeleft = None
		self.returncode = None

	def getargs(self):
		pass

	def getlogfile(self):
		return self.name() + ".log"

	def start(self):
		assert(self.state == Service.Stopped)

		try:
			logfile = open(os.path.join(self.logdir, self.getlogfile()), "w")
			self.proc = subprocess.Popen(self.getargs(), stdout=logfile, stderr=subprocess.STDOUT)
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

		print "process"
		try:
			if self.proc.poll() is not None:
				print self.proc.returncode
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
	def __init__(self, binpath, configpath, logdir):
		super(Mongrel2Service, self).__init__(logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "mongrel2"

	def start(self):
		path, ext = os.path.splitext(self.configpath)
		self.sqlconfigpath = path + ".sqlite"

		# generate sqlite config
		try:
			subprocess.check_call(["m2sh", "load", "-config", self.configpath, "-db", self.sqlconfigpath])
		except:
			return False

		return super(Mongrel2Service, self).start()

	def getargs(self):
		return [self.binpath, self.sqlconfigpath, self.serverid]

class ZurlService(SingleProcessService):
	def __init__(self, binpath, configpath, logdir):
		super(ZurlService, self).__init__(logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "zurl"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

class PushpinProxyService(SingleProcessService):
	def __init__(self, binpath, configpath, logdir):
		super(PushpinProxyService, self).__init__(logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "pushpin-proxy"

	def getargs(self):
		return [self.binpath, "--config=%s" % self.configpath]

class PushpinHandlerService(SingleProcessService):
	def __init__(self, binpath, configpath, logdir):
		super(PushpinHandlerService, self).__init__(logdir)
		self.binpath = binpath
		self.configpath = configpath

	def name(self):
		return "pushpin-handler"

	def getargs(self):
		return ["python", self.binpath, "--config=%s" % self.configpath]

import time
import subprocess
import atexit
import signal
import copy

class Process(object):
	Stopped, Started, Stopping = range(3)

	def __init__(self, name):
		self.state = Process.Stopped
		self.proc = None
		self.timeleft = None
		self.returncode = None
		self.name = name

	def start(self, args, logfile=None):
		assert(self.state == Process.Stopped)
		print "starting %s" % self.name
		if not logfile:
			logfile = "/dev/null"
		fp = open(logfile, "w")
		self.proc = subprocess.Popen(args, stdout=fp, stderr=subprocess.STDOUT)
		self.state = Process.Started

	def stop(self):
		assert(self.state == Process.Started)
		self.state = Process.Stopping
		print "stopping %s" % self.name
		self.proc.terminate()
		self.timeleft = 10

	# call once per second
	def check(self):
		assert(self.state == Process.Started or self.state == Process.Stopping)
		if self.proc.poll() is not None:
			if self.state != Process.Stopping:
				raise RuntimeError("process exited unexpectedly: %s" % self.name)
			self.returncode = self.proc.returncode
			self.state = Process.Stopped
		elif self.state == Process.Stopping:
			if self.timeleft <= 0:
				print "warning: %s taking too long, forcing quit" % self.name
				self.proc.kill()
				self.state = Service.Stopped
				self.timeleft = None
			else:
				self.timeleft -= 1

	def is_stopped(self):
		return self.state == Process.Stopped

class ProcessManager(object):
	def __init__(self):
		self.procs = list()
		self.raw_procs = set()
		self.quit = False
		self.stopmessage = None
		atexit.register(self.cleanup)

	def add(self, name, args, logfile=None):
		if len(self.procs) == 0:
			signal.signal(signal.SIGTERM, self.termfunc)
			signal.signal(signal.SIGINT, self.termfunc)
		p = Process(name)
		p.start(args, logfile)
		self.procs.append(p)
		self.raw_procs.add(p.proc)
		return p.proc.pid

	def wait(self):
		# wait for ctrl-c or sigterm
		while not self.quit:
			for p in self.procs:
				p.check()
			time.sleep(1)

		if self.stopmessage:
			print self.stopmessage

		# graceful terminate
		for p in self.procs:
			p.stop()

		while True:
			all_stopped = True
			for p in copy.copy(self.procs):
				p.check()
				if p.is_stopped():
					self.procs.remove(p)
					self.raw_procs.remove(p.proc)
				else:
					all_stopped = False
			if all_stopped:
				break
			time.sleep(1)

		assert(len(self.procs) == 0)
		assert(len(self.raw_procs) == 0)

	def termfunc(self, signum, frame):
		signal.signal(signal.SIGTERM, signal.SIG_DFL)
		signal.signal(signal.SIGINT, signal.SIG_DFL)
		self.quit = True

	def cleanup(self):
		for p in self.raw_procs:
			if p.returncode is None:
				p.kill()

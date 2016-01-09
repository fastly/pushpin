import os
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
		self.accept_sighup = False

	def start(self, args, logfile=None, accept_sighup=False):
		assert(self.state == Process.Stopped)
		self.accept_sighup = accept_sighup
		print "starting %s" % self.name
		if not logfile:
			logfile = "/dev/null"
		fp = open(logfile, "w")
		self.proc = subprocess.Popen(args, stdout=fp, stderr=subprocess.STDOUT, preexec_fn=self._preexec_fn)
		self.state = Process.Started

	def stop(self):
		assert(self.state == Process.Started)
		self.state = Process.Stopping
		print "stopping %s" % self.name
		self.proc.terminate()
		self.timeleft = 16 # 8 seconds

	# call frequently after stopping to detect stopped
	def check(self):
		assert(self.state == Process.Started or self.state == Process.Stopping)
		if self.proc.poll() is not None:
			if self.state != Process.Stopping:
				raise RuntimeError("process exited unexpectedly: %s" % self.name)
			self.returncode = self.proc.returncode
			if self.returncode != 0 and self.returncode != -15:
				print "warning: %s unexpected return code: %d" % (self.name, self.returncode)
			self.state = Process.Stopped
		elif self.state == Process.Stopping:
			if self.timeleft <= 0:
				print "warning: %s taking too long, forcing quit" % self.name
				self.proc.kill()
				self.proc.wait()
				self.state = Process.Stopped
				self.timeleft = None
			else:
				self.timeleft -= 1

	def is_stopped(self):
		return self.state == Process.Stopped

	def _preexec_fn(self):
		os.setpgrp()

class ProcessManager(object):
	def __init__(self):
		self.procs = list()
		self.raw_procs = set()
		self.quit = False
		self.send_hup = False
		self.reloadmessage = None
		self.stopmessage = None
		atexit.register(self.cleanup)

	def add(self, name, args, logfile=None, accept_sighup=False):
		if len(self.procs) == 0:
			self.prev_sighup = signal.getsignal(signal.SIGHUP)
			self.prev_sigint = signal.getsignal(signal.SIGINT)
			self.prev_sigterm = signal.getsignal(signal.SIGTERM)
			signal.signal(signal.SIGHUP, self.termfunc)
			signal.signal(signal.SIGINT, self.termfunc)
			signal.signal(signal.SIGTERM, self.termfunc)
		p = Process(name)
		p.start(args, logfile, accept_sighup)
		self.procs.append(p)
		self.raw_procs.add(p.proc)
		return p.proc.pid

	def wait(self):
		# wait for ctrl-c or sigterm
		while not self.quit:
			for p in self.procs:
				p.check()
			if self.send_hup:
				self.send_hup = False
				for p in self.procs:
					if p.accept_sighup:
						p.proc.send_signal(signal.SIGHUP)
			time.sleep(1)

		if self.stopmessage:
			print self.stopmessage

		self.quit = False

		# gracefully terminate
		for p in self.procs:
			p.stop()

		while not self.quit:
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
			time.sleep(0.1)

		# if we quit early here, then kill remaining processes
		for p in copy.copy(self.procs):
			if p.proc.returncode is None:
				p.proc.kill()
				p.proc.wait()
			self.procs.remove(p)
			self.raw_procs.remove(p.proc)

		assert(len(self.procs) == 0)
		assert(len(self.raw_procs) == 0)

	def termfunc(self, signum, frame):
		# we need to leave the signal handlers enabled so the user
		#   can't abort during cleanup
		#signal.signal(signal.SIGHUP, self.prev_sighup)
		#signal.signal(signal.SIGINT, self.prev_sigint)
		#signal.signal(signal.SIGTERM, self.prev_sigterm)
		if signum == signal.SIGHUP:
			self.send_hup = True
			if self.reloadmessage:
				print self.reloadmessage
		else:
			self.quit = True

	def cleanup(self):
		for p in self.raw_procs:
			if p.returncode is None:
				p.kill()
				p.wait()

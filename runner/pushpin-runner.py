import sys
import os
import time
import signal
import ConfigParser
import services

config_file = "/etc/pushpin/pushpin.conf"
for arg in sys.argv:
	if arg.startswith("--config="):
		config_file = arg[9:]
		break

config = ConfigParser.ConfigParser()
config.read([config_file])

configdir = config.get("runner", "configdir")
if not os.path.isabs(configdir):
	configdir = os.path.join(os.path.dirname(config_file), configdir)

logdir = config.get("runner", "logdir")
if not os.path.isabs(logdir):
	logdir = os.path.join(os.path.dirname(config_file), logdir)

exedir = os.path.dirname(os.path.realpath(__file__))

proxybin = "pushpin-proxy"
path = os.path.normpath(os.path.join(exedir, "../proxy/pushpin-proxy"))
if os.path.isfile(path):
	proxybin = path

handlerbin = "pushpin-handler.py"
path = os.path.normpath(os.path.join(exedir, "../handler/pushpin-handler.py"))
if os.path.isfile(path):
	handlerbin = path

service_names = config.get("runner", "services").split(",")

service_objs = list()
for name in service_names:
	if name == "mongrel2":
		service_objs.append(services.Mongrel2Service("mongrel2", os.path.join(configdir, "mongrel2.conf"), logdir))
	elif name == "zurl":
		service_objs.append(services.ZurlService("zurl", os.path.join(configdir, "zurl.conf"), logdir))
	elif name == "pushpin-proxy":
		service_objs.append(services.PushpinProxyService(proxybin, config_file, logdir))
	elif name == "pushpin-handler":
		service_objs.append(services.PushpinHandlerService(handlerbin, config_file, logdir))

quit = False

def termfunc(signum, frame):
	global quit
	quit = True

signal.signal(signal.SIGTERM, termfunc)
signal.signal(signal.SIGINT, termfunc)

print "starting..."

for s in service_objs:
	print "starting %s" % s.name()
	if not s.start():
		print "error starting %s" % s.name()

print "started"

while not quit:
	for s in service_objs:
		if not s.process():
			print "error processing %s" % s.name()
	time.sleep(1)

print "stopping..."

for s in service_objs:
	print "stopping %s" % s.name()
	s.stop()

while True:
	all_stopped = True
	for s in service_objs:
		if not s.process():
			print "error processing %s" % s.name()
		if not s.is_stopped():
			all_stopped = False
	if all_stopped:
		break
	time.sleep(1)

print "stopped"

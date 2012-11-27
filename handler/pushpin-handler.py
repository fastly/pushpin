import sys
import time
import threading
import ConfigParser
import zmq
import tnetstring

config_file = "/etc/pushpin.conf"
for arg in sys.argv:
	if arg.startswith("--config="):
		config_file = arg[9:]
		break

config = ConfigParser.ConfigParser()
config.read([config_file])

ctx = zmq.Context()

def inspect_worker():
	sock = ctx.socket(zmq.REP)
	sock.connect(config.get("handler", "proxy_inspect_spec"))

	while True:
		m_raw = sock.recv()
		m = tnetstring.loads(m_raw)
		print "IN: %s" % m

		# reply saying to always proxy
		id = m["id"]
		m = dict()
		m["id"] = id
		m["no-proxy"] = False

		print "OUT: %s" % m
		m_raw = tnetstring.dumps(m)
		sock.send(m_raw)

def accept_worker():
	sock = ctx.socket(zmq.PULL)
	sock.connect(config.get("handler", "proxy_accept_in_spec"))

	while True:
		m_raw = sock.recv()
		m = tnetstring.loads(m_raw)
		print "IN: %s" % m
		print "for now, dropping"

inspect_thread = threading.Thread(target=inspect_worker)
inspect_thread.start()

accept_thread = threading.Thread(target=accept_worker)
accept_thread.start()

try:
	while True:
		time.sleep(60)
except KeyboardInterrupt:
	pass

ctx.term()

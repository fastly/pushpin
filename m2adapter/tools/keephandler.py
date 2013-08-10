# this handler doesn't respond to requests, but will send keep-alives forever

import time
import threading
import tnetstring
import zmq

class Session(object):
	def __init__(self):
		self.to_address = None
		self.out_seq = 0

sessions = dict()
lock = threading.Lock()

client_id = 'zhttp-test'

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc:///tmp/zhttp-test-out')
out_sock = ctx.socket(zmq.PUB)
out_sock.connect('ipc:///tmp/zhttp-test-in')

def keepalive_worker():
	lock.acquire()
	for id, s in sessions.iteritems():
		resp = dict()
		resp['from'] = client_id
		resp['id'] = id
		resp['seq'] = s.out_seq
		s.out_seq += 1
		resp['type'] = 'credit'
		resp['credits'] = 0

		print 'OUT %s' % resp
		out_sock.send(s.to_address + ' T' + tnetstring.dumps(resp))
	lock.release()

class KeepAliveThread(threading.Thread):
	def run(self):
		while True:
			keepalive_worker()
			time.sleep(30)

keepalive_thread = KeepAliveThread()
keepalive_thread.daemon = True
keepalive_thread.start()

while True:
	m_raw = in_sock.recv()
	req = tnetstring.loads(m_raw[1:])
	print 'IN %s' % req

	id = req['id']
	if id in sessions:
		print 'session already active'
		continue

	if 'type' in req:
		print 'wrong packet type'
		continue

	s = Session()
	s.to_address = req['from']
	lock.acquire()
	sessions[id] = s
	lock.release()

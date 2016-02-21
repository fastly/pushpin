# this handler just eats requests and never responds. what a jerk!

import tnetstring
import zmq

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc:///tmp/zhttp-test-out')

while True:
	m_raw = in_sock.recv()
	req = tnetstring.loads(m_raw[1:])
	print 'IN %s' % req

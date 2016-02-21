# this handler responds to every request with "hello world"

import tnetstring
import zmq

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc:///tmp/zhttp-test-out')
out_sock = ctx.socket(zmq.PUB)
out_sock.connect('ipc:///tmp/zhttp-test-in')

while True:
	m_raw = in_sock.recv()
	req = tnetstring.loads(m_raw[1:])
	print 'IN %s' % req

	resp = dict()
	resp['id'] = req['id']
	resp['code'] = 200
	resp['reason'] = 'OK'
	resp['headers'] = [['Content-Type', 'text/plain']]
	resp['body'] = 'hello world\n'

	print 'OUT %s' % resp
	out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))

# this handler echos every frame

import tnetstring
import zmq

instance_id = 'wsechohandler'

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc:///tmp/zhttp-test-out')
in_stream_sock = ctx.socket(zmq.ROUTER)
in_stream_sock.setsockopt(zmq.IDENTITY, instance_id)
in_stream_sock.connect('ipc:///tmp/zhttp-test-out-stream')
out_sock = ctx.socket(zmq.PUB)
out_sock.connect('ipc:///tmp/zhttp-test-in')

poller = zmq.Poller()
poller.register(in_sock, zmq.POLLIN)
poller.register(in_stream_sock, zmq.POLLIN)

sessions = set()

while True:
	socks = dict(poller.poll())
	if socks.get(in_sock) == zmq.POLLIN:
		m_raw = in_sock.recv()
		req = tnetstring.loads(m_raw[1:])
	elif socks.get(in_stream_sock) == zmq.POLLIN:
		m_raw = in_stream_sock.recv_multipart()
		req = tnetstring.loads(m_raw[2][1:])
	else:
		continue

	print 'IN %s' % req

	rid = (req['from'], req['id'])

	resp = dict()
	if rid not in sessions:
		rtype = req.get('type')

		# first packet must be a data packet
		if rtype is not None:
			continue

		sessions.add(rid)
		resp['credits'] = 200000
	else:
		rtype = req.get('type')
		if rtype is None:
			if 'content-type' in req:
				resp['content-type'] = req['content-type']
			resp['body'] = req['body']
			count = len(req['body'])
			if req.get('more'):
				resp['more'] = True
			resp['credits'] = len(req['body'])
		elif rtype == 'close':
			sessions.remove(rid)
			resp['type'] = 'close'
			if 'code' in req:
				resp['code'] = req['code']
		elif rtype == 'keep-alive':
			resp['type'] = 'keep-alive'
		else:
			continue

	resp['from'] = instance_id
	resp['id'] = req['id']
	print 'OUT %s' % resp
	out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))

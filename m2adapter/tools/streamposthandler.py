# this handler accepts a POST and responds with it

import tnetstring
import zmq

class Session(object):
	def __init__(self):
		self.id = None
		# 0 = receiving, 1 = responding
		self.state = 0
		self.sent_credits = False
		self.content_type = None
		self.data = ''
		self.offset = 0
		self.out_seq = 0
		self.credits = 0

	def take_data(self):
		left = len(self.data) - self.offset
		if self.credits >= left:
			take = left
		else:
			take = self.credits
		body = self.data[self.offset:self.offset + take]
		self.offset += take
		self.credits -= take
		return body

sessions = dict()

client_id = 'zhttp-test'

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc:///tmp/zhttp-test-out')
in_stream_sock = ctx.socket(zmq.DEALER)
in_stream_sock.setsockopt(zmq.IDENTITY, client_id)
in_stream_sock.connect('ipc:///tmp/zhttp-test-out-stream')
out_sock = ctx.socket(zmq.PUB)
out_sock.connect('ipc:///tmp/zhttp-test-in')

poller = zmq.Poller()
poller.register(in_sock, zmq.POLLIN)
poller.register(in_stream_sock, zmq.POLLIN)

while True:
	socks = dict(poller.poll())
	if socks.get(in_sock) == zmq.POLLIN:
		m_raw = in_sock.recv()
		req = tnetstring.loads(m_raw[1:])
		print 'IN %s' % req

		id = req['id']
		if id in sessions:
			print 'session already active'
			continue

		s = Session()
		s.id = id
		headers = req.get('headers')
		if headers:
			for h in headers:
				if h[0].lower() == 'content-type':
					s.content_type = h[1]
					break
		sessions[id] = s

	elif socks.get(in_stream_sock) == zmq.POLLIN:
		parts = in_stream_sock.recv_multipart()
		req = tnetstring.loads(parts[1][1:])
		print 'IN stream %s' % req

		s = sessions.get(id)
		if s is None:
			print 'no such session'
			continue

	ptype = req.get('type')
	if ptype is None or (ptype is not None and ptype == 'credit'):
		if 'credits' in req:
			s.credits += req['credits']

	if ptype is None:
		assert(s.state == 0)
		body = ''
		if 'body' in req:
			body = req['body']

		s.data += body

		if not req.get('more'):
			s.state = 1 # responding

			resp = dict()
			resp['from'] = client_id
			resp['id'] = s.id
			resp['seq'] = s.out_seq
			s.out_seq += 1
			resp['code'] = 200
			resp['reason'] = 'OK'
			headers = list()
			if s.content_type:
				headers.append(['Content-Type', s.content_type])
			headers.append(['Content-Length', str(len(s.data))])
			resp['headers'] = headers
			resp['body'] = s.take_data()
			if s.offset < len(s.data):
				resp['more'] = True
			else:
				del sessions[s.id]

			print 'OUT %s' % resp
			out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))
		else:
			# send credits
			resp = dict()
			resp['from'] = client_id
			resp['id'] = s.id
			resp['seq'] = s.out_seq
			s.out_seq += 1
			resp['type'] = 'credit'
			if not s.sent_credits:
				resp['credits'] = 200000
				s.sent_credits = True
			else:
				resp['credits'] = len(body)

			print 'OUT %s' % resp
			out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))
	elif ptype is not None and ptype == 'credit':
		if s.state == 1 and s.credits > 0:
			resp = dict()
			resp['from'] = client_id
			resp['id'] = id
			resp['seq'] = s.out_seq
			s.out_seq += 1
			resp['body'] = s.take_data()
			if s.offset < len(s.data):
				resp['more'] = True
			else:
				del sessions[s.id]

			print 'OUT %s' % resp
			out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))

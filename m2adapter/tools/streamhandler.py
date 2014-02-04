# this handler responds with a file

import sys
import os
import tnetstring
import zmq

class Session(object):
	def __init__(self):
		self.file = None
		self.out_seq = 0
		self.credits = 0

filename = sys.argv[1]

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

		if 'type' in req:
			print 'wrong packet type'
			continue

		s = Session()
		sessions[id] = s

		s.file = open(filename, 'r')

		if 'credits' in req:
			s.credits += req['credits']

		body = ''
		eof = False
		while s.credits > 0:
			buf = s.file.read(s.credits)
			s.credits -= len(buf)
			body += buf
			if len(buf) == 0:
				eof = True
				break

		resp = dict()
		resp['from'] = client_id
		resp['id'] = req['id']
		resp['seq'] = s.out_seq
		resp['code'] = 200
		resp['reason'] = 'OK'
		headers = list()
		headers.append(['Content-Type', 'text/plain'])
		headers.append(['Content-Length', str(os.stat(filename).st_size)])
		resp['headers'] = headers
		if len(body) > 0:
			resp['body'] = body
		if not eof:
			resp['more'] = True

		s.out_seq += 1

		if eof:
			del sessions[id]

		print 'OUT %s' % resp
		out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))
	elif socks.get(in_stream_sock) == zmq.POLLIN:
		parts = in_stream_sock.recv_multipart()
		req = tnetstring.loads(parts[1][1:])
		print 'IN stream %s' % req

		# we are only streaming output, so subsequent input messages must be credits
		if 'type' not in req or req['type'] != 'credit':
			print 'wrong packet type'
			continue

		id = req['id']

		s = sessions.get(id)
		if s is None:
			continue

		if 'credits' in req:
			s.credits += req['credits']

		body = ''
		eof = False
		while s.credits > 0:
			buf = s.file.read(s.credits)
			s.credits -= len(buf)
			body += buf
			if len(buf) == 0:
				eof = True
				break

		resp = dict()
		resp['from'] = client_id
		resp['id'] = id
		resp['seq'] = s.out_seq

		s.out_seq += 1

		if len(body) > 0:
			resp['body'] = body
		if not eof:
			resp['more'] = True

		if eof:
			del sessions[id]

		print 'OUT %s' % resp
		out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))

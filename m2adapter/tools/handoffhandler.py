# this handler will start a handoff and then reclaim it 10 seconds later

import time
import tnetstring
import zmq

client_id = 'zhttp-test'

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc:///tmp/zhttp-test-out')
in_stream_sock = ctx.socket(zmq.ROUTER)
in_stream_sock.identity = client_id
in_stream_sock.connect('ipc:///tmp/zhttp-test-out-stream')
out_sock = ctx.socket(zmq.PUB)
out_sock.connect('ipc:///tmp/zhttp-test-in')

while True:
	m_raw = in_sock.recv()
	req = tnetstring.loads(m_raw[1:])
	print 'IN %s' % req

	out_seq = 0

	resp = dict()
	resp['from'] = client_id
	resp['id'] = req['id']
	resp['seq'] = out_seq
	out_seq += 1
	resp['type'] = 'handoff-start'
	print 'OUT %s' % resp
	out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))

	m_raw = in_stream_sock.recv_multipart()
	req = tnetstring.loads(m_raw[2][1:])
	print 'IN %s' % req

	assert(req['type'] == 'handoff-proceed')

	time.sleep(10)

	resp = dict()
	resp['from'] = client_id
	resp['id'] = req['id']
	resp['seq'] = out_seq
	out_seq += 1
	resp['type'] = 'keep-alive'
	print 'OUT %s' % resp
	out_sock.send(req['from'] + ' T' + tnetstring.dumps(resp))

	m_raw = in_stream_sock.recv_multipart()
	req = tnetstring.loads(m_raw[2][1:])
	print 'IN %s' % req

	assert(req['type'] == 'error')
	print 'error: %s' % req['condition']

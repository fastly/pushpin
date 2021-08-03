import sys
import json
import tnetstring
import zmq

ctx = zmq.Context()
sock = ctx.socket(zmq.SUB)
sock.connect(sys.argv[1])

if len(sys.argv) >= 3:
	for mtype in sys.argv[2].split(','):
		sock.setsockopt(zmq.SUBSCRIBE, '{} '.format(mtype))
else:
	sock.setsockopt(zmq.SUBSCRIBE, '')

while True:
	m_raw = sock.recv()
	at = m_raw.find(' ')
	mtype = m_raw[:at]
	mdata = m_raw[at + 1:]
	if mdata[0] == 'T':
		m = tnetstring.loads(mdata[1:])
	elif mdata[0] == 'J':
		m = json.loads(mdata[1:])
	else:
		m = mdata
	print '%s %s' % (mtype, m)

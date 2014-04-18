import sys
import tnetstring
import zmq

ctx = zmq.Context()
sock = ctx.socket(zmq.SUB)
sock.connect(sys.argv[1])
sock.setsockopt(zmq.SUBSCRIBE, '')

while True:
	m_raw = sock.recv()
	at = m_raw.find(' ')
	mtype = m_raw[:at]
	mdata = m_raw[at + 1:]
	if mdata[0] == 'T':
		m = tnetstring.loads(mdata[1:])
	else:
		m = tnetstring.loads(mdata)
	print '%s %s' % (mtype, m)

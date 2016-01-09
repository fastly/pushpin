import sys
import zmq

if len(sys.argv) < 2:
	print 'usage: %s [pub_spec]' % sys.argv[0]
	sys.exit(1)

spec = sys.argv[1]

zmq_context = zmq.Context.instance()
sock = zmq_context.socket(zmq.XPUB)
sock.rcvhwm = 0
if hasattr(sock, 'immediate'):
	sock.immediate = 1
sock.connect(spec)

while True:
	m = sock.recv()
	mtype = m[0]
	topic = m[1:]
	if mtype == '\x01':
		print 'SUB %s' % topic
	elif mtype == '\x00':
		print 'UNSUB %s' % topic

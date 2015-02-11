import sys
import tnetstring
import zmq

if len(sys.argv) < 3:
	print 'usage: %s [pub_spec] [pull_spec]' % sys.argv[0]
	sys.exit(1)

pub_spec = sys.argv[1]
pull_spec = sys.argv[2]

zmq_context = zmq.Context()

pull_sock = zmq_context.socket(zmq.PULL)
pull_sock.bind(pull_spec)

pub_sock = zmq_context.socket(zmq.XPUB)
pub_sock.bind(pub_spec)

poller = zmq.Poller()
poller.register(pull_sock, zmq.POLLIN)
poller.register(pub_sock, zmq.POLLIN)

subs = set()

while True:
	socks = dict(poller.poll())
	if socks.get(pull_sock) == zmq.POLLIN:
		m = tnetstring.loads(pull_sock.recv())
		channel = m['channel']
		if channel in subs:
			del m['channel']
			pub_sock.send_multipart([channel, tnetstring.dumps(m)])
	elif socks.get(pub_sock) == zmq.POLLIN:
		m = pub_sock.recv()
		mtype = m[0]
		topic = m[1:]
		if mtype == '\x01':
			assert(topic not in subs)
			print 'subscribing [%s]' % topic
			subs.add(topic)
		elif mtype == '\x00':
			assert(topic in subs)
			print 'unsubscribing [%s]' % topic
			subs.remove(topic)

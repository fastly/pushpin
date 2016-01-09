import sys
import time
import tnetstring
import zmq

class Subscription(object):
	def __init__(self):
		self.ttl = None
		self.last_refresh = None

# key=(mode, channel)
subs = dict()

ctx = zmq.Context()
sock = ctx.socket(zmq.SUB)
sock.connect(sys.argv[1])
sock.setsockopt(zmq.SUBSCRIBE, 'sub ')

poller = zmq.Poller()
poller.register(sock, zmq.POLLIN)

while True:
	socks = dict(poller.poll(1000))
	if socks.get(sock) == zmq.POLLIN:
		m_raw = sock.recv()
		if not m_raw.startswith('sub T'):
			continue
		m = tnetstring.loads(m_raw[5:])

		now = int(time.time())
		sub_key = (m['mode'], m['channel'])
		sub = subs.get(sub_key)
		if m.get('unavailable'):
			if sub:
				del subs[sub_key]
				print 'UNSUB mode=%s channel=%s' % (m['mode'], m['channel'])
		else:
			if not sub:
				sub = Subscription()
				subs[sub_key] = sub
				print 'SUB mode=%s channel=%s' % (m['mode'], m['channel'])
			sub.ttl = m['ttl']
			sub.last_refresh = now

	unsubs = set()
	for sub_key, sub in subs.iteritems():
		if sub.last_refresh + sub.ttl <= now:
			unsubs.add(sub_key)
	for sub_key in unsubs:
		del subs[sub_key]
		print 'UNSUB mode=%s channel=%s' % (sub_key[0], sub_key[1])

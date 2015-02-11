# this program uses pushpin's PULL socket to publish

import sys
import tnetstring
import zmq

args = sys.argv[1:]

spec = 'tcp://localhost:5560'

n = 0
while n < len(args):
	arg = args[n]
	if arg.startswith('--spec='):
		spec = arg[7:]
		del args[n]
		n -= 1 # adjust position
	n += 1

if len(args) < 2:
	print 'usage: %s (--spec=x) [channel] [content]' % sys.argv[0]
	sys.exit(1)

channel = args[0]
content = args[1]

ctx = zmq.Context()
sock = ctx.socket(zmq.PUSH)
sock.connect(spec)

hr = dict()
hr['body'] = content + '\n'
hs = dict()
hs['content'] = content + '\n'
ws = dict()
ws['content'] = content
formats = dict()
formats['http-response'] = hr
formats['http-stream'] = hs
formats['ws-message'] = ws
item = dict()
item['channel'] = channel
item['formats'] = formats

sock.send(tnetstring.dumps(item))

print 'Published'

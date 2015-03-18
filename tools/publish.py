# this program uses pushpin's PULL socket to publish

import argparse
import tnetstring
import zmq

parser = argparse.ArgumentParser(description='Publish messages to Pushpin.')
parser.add_argument('channel', help='channel to send to')
parser.add_argument('content',
	help='content to use for HTTP body and WS message')
parser.add_argument('--code', type=int,
	help='HTTP response code to use. default 200')
parser.add_argument('-H', '--header', action='append',
	help='add HTTP response header')
parser.add_argument('--spec', default='tcp://localhost:5560',
	help='zmq PUSH spec. default tcp://localhost:5560')
args = parser.parse_args()

headers = []
if args.header:
	for h in args.header:
		k, v = h.split(':')
		headers.append([k, v.lstrip()])

ctx = zmq.Context()
sock = ctx.socket(zmq.PUSH)
sock.connect(args.spec)

hr = {'body': args.content + '\n'}
if args.code is not None:
	hr['code'] = args.code
if headers:
	hr['headers'] = headers
hs = {'content': args.content + '\n'}
ws = {'content': args.content}

item = {
	'channel': args.channel,
	'formats': {
		'http-response': hr,
		'http-stream': hs,
		'ws-message': ws
	}
}

sock.send(tnetstring.dumps(item))

print 'Published'

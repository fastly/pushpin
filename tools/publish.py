#!/usr/bin/env python
#
# Copyright (C) 2015 Fanout, Inc.
#
# This file is part of Pushpin.
#
# Pushpin is free software: you can redistribute it and/or modify it under
# the terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
# more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

import sys
import json
import argparse
import tnetstring
import zmq

def ensure_utf8(i):
	if isinstance(i, dict):
		out = {}
		for k, v in i.iteritems():
			out[ensure_utf8(k)] = ensure_utf8(v)
		return out
	elif isinstance(i, list):
		out = []
		for v in i:
			out.append(ensure_utf8(v))
		return out
	elif isinstance(i, unicode):
		return i.encode("utf-8")
	else:
		return i

parser = argparse.ArgumentParser(description='Publish messages to Pushpin.')
parser.add_argument('channel', help='channel to send to')
parser.add_argument('content', nargs='?', default='',
	help='content to use for HTTP body and WS message')
parser.add_argument('--code', type=int,
	help='HTTP response code to use. default 200')
parser.add_argument('-H', '--header', action='append',
	help='add HTTP response header')
parser.add_argument('--spec', default='tcp://localhost:5560',
	help='zmq PUSH spec. default tcp://localhost:5560')
parser.add_argument('--close', action='store_true',
	help='close streaming requests')
parser.add_argument('--id',
	help='payload ID')
parser.add_argument('--prev-id',
	help='payload previous ID')
parser.add_argument('--sender',
	help='sender meta value')
parser.add_argument('--patch', action='store_true',
	help='content is JSON patch')
args = parser.parse_args()

headers = []
if args.header:
	for h in args.header:
		k, v = h.split(':', 1)
		headers.append([k, v.lstrip()])

meta = dict()
formats = dict()

if args.content:
	hr = {}
	if args.patch:
		hr['body-patch'] = ensure_utf8(json.loads(args.content))
	else:
		hr['body'] = args.content + '\n'
	if args.code is not None:
		hr['code'] = args.code
	if headers:
		hr['headers'] = headers
	formats['http-response'] = hr

if args.close:
	formats['http-stream'] = {'action': 'close'}
elif args.content and not args.patch:
	formats['http-stream'] = {'content': args.content + '\n'}

if args.content and not args.patch:
	formats['ws-message'] = {'content': args.content}

if not formats:
	print 'error: nothing to send'
	sys.exit(1)

if args.sender:
	meta['sender'] = args.sender

item = {
	'channel': args.channel,
	'formats': formats
}

if args.id:
	item['id'] = args.id
if args.prev_id:
	item['prev-id'] = args.prev_id

if meta:
	item['meta'] = meta

ctx = zmq.Context()
sock = ctx.socket(zmq.PUSH)
sock.connect(args.spec)

sock.send(tnetstring.dumps(item))

print 'Published'

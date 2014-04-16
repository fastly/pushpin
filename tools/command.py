import sys
import uuid
import json
import tnetstring
import zmq

def make_tnet_compat(obj):
	if isinstance(obj, dict):
		out = dict()
		for k, v in obj.iteritems():
			out[make_tnet_compat(k)] = make_tnet_compat(v)
		return out
	elif isinstance(obj, list):
		out = list()
		for v in obj:
			out.append(make_tnet_compat(v))
		return out
	elif isinstance(obj, unicode):
		return obj.encode('utf-8')
	else:
		return out

ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect(sys.argv[1])

req = dict()
req['id'] = str(uuid.uuid4())
req['method'] = sys.argv[2]
if len(sys.argv) > 3:
	args = json.loads(sys.argv[3])
	assert(isinstance(args, dict))
	req['args'] = make_tnet_compat(args)
else:
	req['args'] = dict()
print 'calling %s: args=%s' % (req['method'], req['args'])
sock.send(tnetstring.dumps(req))

resp = tnetstring.loads(sock.recv())
if resp['success']:
	print 'success: %s' % resp['value']
else:
	print 'error: %s' % resp['condition']

import sys
import zmq
import tnetstring

command_host = None

def resolve(uri):
	if uri.startswith('tcp://'):
		at = uri.find(':', 6)
		addr = uri[6:at]
		if addr == '*':
			if command_host:
				return uri[0:6] + command_host + uri[at:]
			else:
				return uri[0:6] + 'localhost' + uri[at:]
	return uri

command_uri = sys.argv[1]
if command_uri.startswith('tcp://'):
	at = command_uri.find(':', 6)
	command_host = command_uri[6:at]

sock = zmq.Context.instance().socket(zmq.REQ)
sock.connect(command_uri)

req = {'method': 'get-zmq-uris'}
sock.send(tnetstring.dumps(req))

resp = tnetstring.loads(sock.recv())
if not resp.get('success'):
	raise ValueError('request failed: %s' % resp)

v = resp['value']

if 'command' in v:
	print 'command: %s' % resolve(v['command'])
if 'publish-pull' in v:
	print 'publish-pull: %s' % resolve(v['publish-pull'])
if 'publish-sub' in v:
	print 'publish-sub: %s' % resolve(v['publish-sub'])

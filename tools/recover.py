import sys
import zmq
import tnetstring

command_uri = sys.argv[1]

sock = zmq.Context.instance().socket(zmq.REQ)
sock.connect(command_uri)

req = {'method': 'recover'}
sock.send(tnetstring.dumps(req))

resp = tnetstring.loads(sock.recv())
if not resp.get('success'):
	raise ValueError('request failed: %s' % resp)

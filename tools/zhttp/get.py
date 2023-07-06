import sys
import uuid
import tnetstring
import zmq

if len(sys.argv) < 2:
    print('usage: {} [url]'.format(sys.argv[0]))
    sys.exit(1)

ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect('ipc://server')

req = {
    b'method': b'GET',
    b'uri': sys.argv[1].encode('utf-8'),
    #b'follow-redirects': True,
    #b'ignore-tls-errors': True,
}

sock.send(b'T' + tnetstring.dumps(req))

resp = tnetstring.loads(sock.recv()[1:])
if b'type' in resp and resp[b'type'] == b'error':
    print('error: {}'.format(resp[b'condition']))
    sys.exit(1)

print('code={} reason=[{}]'.format(resp[b'code'], resp[b'reason']))
for h in resp[b'headers']:
    print('{}: {}'.format(h[0], h[1]))

if b'body' in resp:
    print('\n{}'.format(resp[b'body']))
else:
    print('\n')

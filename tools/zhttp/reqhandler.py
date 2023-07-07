# this handler responds to every request with "hello world"

import tnetstring
import zmq

ctx = zmq.Context()
sock = ctx.socket(zmq.REP)
sock.connect('ipc://client')

while True:
    m_raw = sock.recv()
    req = tnetstring.loads(m_raw[1:])
    print('IN {}'.format(req))

    resp = {}
    resp[b'id'] = req[b'id']
    resp[b'code'] = 200
    resp[b'reason'] = b'OK'
    resp[b'headers'] = [[b'Content-Type', b'text/plain']]
    resp[b'body'] = b'hello world\n'

    print('OUT {}'.format(resp))
    sock.send(b'T' + tnetstring.dumps(resp))

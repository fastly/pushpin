# this handler responds to every request with "hello world"

import os
import time
import tnetstring
import zmq

instance_id = 'basichandler.{}'.format(os.getpid()).encode()

ctx = zmq.Context()
in_sock = ctx.socket(zmq.PULL)
in_sock.connect('ipc://client-out')
out_sock = ctx.socket(zmq.PUB)
out_sock.connect('ipc://client-in')

# await subscription
time.sleep(0.01)

while True:
    m_raw = in_sock.recv()
    req = tnetstring.loads(m_raw[1:])
    print('IN {}'.format(req))

    resp = {}
    resp[b'from'] = instance_id
    resp[b'id'] = req[b'id']
    resp[b'code'] = 200
    resp[b'reason'] = b'OK'
    resp[b'headers'] = [[b'Content-Type', b'text/plain']]
    resp[b'body'] = b'hello world\n'

    print('OUT {}'.format(resp))
    out_sock.send(req[b'from'] + b' T' + tnetstring.dumps(resp))

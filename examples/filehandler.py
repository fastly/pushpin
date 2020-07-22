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

    f = open('/home/justin/Downloads/node-v12.13.1-linux-x64.tar.xz', 'r')
    #f = open('Cargo.lock', 'r')
    data = f.read()
    f.close()

    resp = {}
    resp[b'id'] = req[b'id']
    resp[b'code'] = 200
    resp[b'reason'] = b'OK'
    resp[b'headers'] = [[b'Content-Type', b'application/octet-stream']]
    resp[b'body'] = data

    print('OUT {}'.format(resp))
    sock.send(b'T' + tnetstring.dumps(resp))

# this handler just outputs the request ID

import tnetstring
import zmq

ctx = zmq.Context()
sock = ctx.socket(zmq.PULL)
sock.connect("ipc://client-out")

while True:
    m = sock.recv_multipart()
    req = tnetstring.loads(m[0][1:])
    print("{} {}".format(req[b"from"].decode(), req[b"id"].decode()))

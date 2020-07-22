# this program sends a response to a certain request ID

import sys
import time
import tnetstring
import zmq

body = sys.argv[1]
addr = sys.argv[2].encode()
rid = sys.argv[3].encode()

ctx = zmq.Context()
sock = ctx.socket(zmq.PUB)
sock.connect('ipc://client-in')

# await subscription
time.sleep(0.01)

resp = {}
resp[b'from'] = b'sendresp'
resp[b'id'] = rid
resp[b'code'] = 200
resp[b'reason'] = b'OK'
resp[b'headers'] = [[b'Content-Type', b'text/plain']]
resp[b'body'] = '{}\n'.format(body).encode()

m = [addr + b' T' + tnetstring.dumps(resp)]

sock.send_multipart(m)

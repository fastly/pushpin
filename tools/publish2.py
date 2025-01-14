# this program uses pushpin's SUB socket to publish

import sys
import time
import tnetstring
import zmq

if len(sys.argv) < 3:
    print("usage: {} [channel] [content]".format(sys.argv[0]))
    sys.exit(1)

channel = sys.argv[1]
content = sys.argv[2]

ctx = zmq.Context()

# note: in a typical long-running program you'd use a normal PUB socket and
# send messages when needed:
#
#   sock = ctx.socket(zmq.PUB)
#   sock.connect(...)
#   ...
#   sock.send_multipart([channel, {item}])
#
# however, since this is a short-running program, we'll use XPUB and wait a
# moment for the subscription to arrive. this is bad practice, though. if you
# need to publish from a short-running program in real life, use pushpin's
# PULL socket, or create a long-running broker that uses PUB to pushpin and
# a PULL socket for input.

sock = ctx.socket(zmq.XPUB)
sock.connect("tcp://localhost:5562")

poller = zmq.Poller()
poller.register(sock, zmq.POLLIN)
start = int(time.time() * 1000)
while True:
    elapsed = int(time.time() * 1000) - start
    if elapsed >= 500:
        # give up
        break
    socks = dict(poller.poll(500 - elapsed))
    if socks.get(sock) == zmq.POLLIN:
        m = sock.recv()
        if m[0] == 1 and m[1:].decode("utf-8") == channel:
            # subscription ready
            break

content = content.encode("utf-8")

hr = {b"body": content + b"\n"}
hs = {b"content": content + b"\n"}
ws = {b"content": content}
formats = {b"http-response": hr, b"http-stream": hs, b"ws-message": ws}
item = {b"formats": formats}

sock.send_multipart([channel.encode("utf-8"), tnetstring.dumps(item)])

print("Published")

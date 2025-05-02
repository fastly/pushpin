import sys
import tnetstring
import zmq

if len(sys.argv) < 3:
    print("usage: {} [pub_spec] [pull_spec]".format(sys.argv[0]))
    sys.exit(1)

pub_spec = sys.argv[1]
pull_spec = sys.argv[2]

zmq_context = zmq.Context()

pull_sock = zmq_context.socket(zmq.PULL)
pull_sock.bind(pull_spec)

pub_sock = zmq_context.socket(zmq.XPUB)
pub_sock.bind(pub_spec)

poller = zmq.Poller()
poller.register(pull_sock, zmq.POLLIN)
poller.register(pub_sock, zmq.POLLIN)

subs = set()

while True:
    socks = dict(poller.poll())
    if socks.get(pull_sock) == zmq.POLLIN:
        m = tnetstring.loads(pull_sock.recv())
        channel = m[b"channel"]
        if channel in subs:
            del m[b"channel"]
            pub_sock.send_multipart([channel, tnetstring.dumps(m)])
    elif socks.get(pub_sock) == zmq.POLLIN:
        m = pub_sock.recv()
        mtype = m[0]
        topic = m[1:]
        if mtype == 1:
            assert topic not in subs
            print("subscribing [{}]".format(topic.decode("utf-8")))
            subs.add(topic)
        elif mtype == 0:
            assert topic in subs
            print("unsubscribing [{}]".format(topic.decode("utf-8")))
            subs.remove(topic)

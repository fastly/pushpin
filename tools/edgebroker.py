import sys
import zmq

if len(sys.argv) < 3:
    print("usage: {} [pub_spec] [sub_spec,sub_spec,...]".format(sys.argv[0]))
    sys.exit(1)

pub_spec = sys.argv[1]
sub_specs = sys.argv[2:]

zmq_context = zmq.Context()

sub_sock = zmq_context.socket(zmq.SUB)
for spec in sub_specs:
    sub_sock.connect(spec)

pub_sock = zmq_context.socket(zmq.XPUB)
pub_sock.connect(pub_spec)

poller = zmq.Poller()
poller.register(sub_sock, zmq.POLLIN)
poller.register(pub_sock, zmq.POLLIN)

while True:
    socks = dict(poller.poll())
    if socks.get(sub_sock) == zmq.POLLIN:
        m = sub_sock.recv_multipart()
        pub_sock.send_multipart(m)
    elif socks.get(pub_sock) == zmq.POLLIN:
        m = pub_sock.recv()
        mtype = m[0]
        topic = m[1:]
        if mtype == 1:
            print("subscribing [{}]".format(topic.decode("utf-8")))
            sub_sock.setsockopt(zmq.SUBSCRIBE, topic)
        elif mtype == 0:
            print("unsubscribing [{}]".format(topic.decode("utf-8")))
            sub_sock.setsockopt(zmq.UNSUBSCRIBE, topic)

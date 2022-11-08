import sys
import zmq

if len(sys.argv) < 2:
    print('usage: {} [pub_spec]'.format(sys.argv[0]))
    sys.exit(1)

spec = sys.argv[1]

zmq_context = zmq.Context.instance()
sock = zmq_context.socket(zmq.XPUB)
sock.rcvhwm = 0
if hasattr(sock, 'immediate'):
    sock.immediate = 1
sock.connect(spec)

while True:
    m = sock.recv()
    mtype = int(m[0])
    topic = m[1:].decode('utf-8')
    if mtype == 1:
        print('SUB {}'.format(topic))
    elif mtype == 0:
        print('UNSUB {}'.format(topic))

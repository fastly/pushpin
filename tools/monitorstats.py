import sys
import json
import tnetstring
import zmq

def ensure_str(i):
    if isinstance(i, dict):
        out = {}
        for k, v in i.items():
            out[ensure_str(k)] = ensure_str(v)
        return out
    elif isinstance(i, list):
        out = []
        for v in i:
            out.append(ensure_str(v))
        return out
    elif isinstance(i, bytes):
        return i.decode('utf-8')
    else:
        return i

ctx = zmq.Context()
sock = ctx.socket(zmq.SUB)
sock.connect(sys.argv[1])

if len(sys.argv) >= 3:
    for mtype in sys.argv[2].split(','):
        sock.setsockopt(zmq.SUBSCRIBE, '{} '.format(mtype).encode('utf-8'))
else:
    sock.setsockopt(zmq.SUBSCRIBE, b'')

while True:
    m_raw = sock.recv()
    at = m_raw.find(b' ')
    mtype = ensure_str(m_raw[:at])
    mdata = m_raw[at + 1:]
    if mdata[0] == ord(b'T'):
        m = ensure_str(tnetstring.loads(mdata[1:]))
    elif mdata[0] == ord(b'J'):
        m = json.loads(mdata[1:])
    else:
        m = mdata
    print('{} {}'.format(mtype, m))

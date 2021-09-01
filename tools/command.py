import sys
import uuid
import json
import tnetstring
import zmq

def make_tnet_compat(obj):
    if isinstance(obj, dict):
        out = {}
        for k, v in obj.items():
            out[make_tnet_compat(k)] = make_tnet_compat(v)
        return out
    elif isinstance(obj, list):
        out = list()
        for v in obj:
            out.append(make_tnet_compat(v))
        return out
    elif isinstance(obj, str):
        return obj.encode('utf-8')
    else:
        return obj

ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect(sys.argv[1])

method = sys.argv[2]

if len(sys.argv) > 3:
    args = json.loads(sys.argv[3])
    assert(isinstance(args, dict))
else:
    args = {}

print('calling {}: args={}'.format(method, repr(args)))

req = {
    b'id': str(uuid.uuid4()).encode('utf-8'),
    b'method': method.encode('utf-8'),
    b'args': make_tnet_compat(args)
}

sock.send(tnetstring.dumps(req))

resp = tnetstring.loads(sock.recv())

if resp[b'success']:
    value = resp[b'value']
    print('success: {}'.format(repr(value)))
else:
    condition = resp[b'condition'].decode('utf-8')
    value = resp.get(b'value')
    print('error: {} {}'.format(condition, repr(value)))

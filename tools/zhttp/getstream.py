import sys
import time
import uuid
import tnetstring
import zmq

client_id = b"getstream.py"

ctx = zmq.Context()
out_sock = ctx.socket(zmq.PUSH)
out_sock.connect("ipc://server-in")
out_stream_sock = ctx.socket(zmq.ROUTER)
out_stream_sock.connect("ipc://server-in-stream")
in_sock = ctx.socket(zmq.SUB)
in_sock.setsockopt(zmq.SUBSCRIBE, client_id)
in_sock.connect("ipc://server-out")

time.sleep(0.5)

rid = str(uuid.uuid4()).encode("utf-8")
inseq = 0
outseq = 0
out_sock.send(
    b"T"
    + tnetstring.dumps(
        {
            b"from": client_id,
            b"id": rid,
            b"seq": outseq,
            b"method": b"GET",
            b"uri": sys.argv[1].encode("utf-8"),
            b"stream": True,
            b"credits": 8192,
        }
    )
)
outseq += 1

while True:
    buf = in_sock.recv()
    at = buf.find(b" ")
    receiver = buf[:at]
    indata = tnetstring.loads(buf[at + 2 :])
    if indata[b"id"] != rid:
        continue
    print("IN: {}".format(indata))
    assert indata[b"seq"] == inseq
    inseq += 1
    if (
        b"type" in indata
        and (indata[b"type"] == b"error" or indata[b"type"] == b"cancel")
    ) or (b"type" not in indata and b"more" not in indata):
        break
    raddr = indata[b"from"]
    if b"body" in indata and len(indata[b"body"]) > 0:
        outdata = {
            b"id": rid,
            b"from": client_id,
            b"seq": outseq,
            b"type": b"credit",
            b"credits": len(indata[b"body"]),
        }
        print("OUT: {}".format(outdata))
        out_stream_sock.send_multipart([raddr, b"", b"T" + tnetstring.dumps(outdata)])
        outseq += 1

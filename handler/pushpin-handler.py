import sys
import time
import threading
import ConfigParser
import json
import zmq
import tnetstring
import httpinterface
from validation import validate_publish, validate_http_publish, ValidationError
from conversion import ensure_utf8, convert_json_transport

config_file = "/etc/pushpin/pushpin.conf"
for arg in sys.argv:
	if arg.startswith("--config="):
		config_file = arg[9:]
		break

config = ConfigParser.ConfigParser()
config.read([config_file])

ctx = zmq.Context()

lock = threading.Lock()
channels = dict()

def header_remove(headers, name):
	for k in headers.keys():
		if k.lower() == name:
			del headers[k]
			return


HTTP_FORMAT = "HTTP/1.1 %(code)s %(status)s\r\n%(headers)s\r\n\r\n%(body)s"

def http_response(body, code, status, headers):
	payload = {"code": code, "status": status, "body": body}
	header_remove(headers, "content-length")
	headers["Content-Length"] = len(body)
	payload["headers"] = "\r\n".join("%s: %s" % (k, v) for k, v in
		headers.items())

	return HTTP_FORMAT % payload

def reply_http(sock, rid, code, status, headers, body):
	header = "%s %d:%s," % (rid[0], len(rid[1]), rid[1])

	if isinstance(status, unicode):
		status = status.encode("utf-8")

	# ensure headers are utf-8
	tmp = dict()
	for k, v in headers.iteritems():
		if isinstance(k, unicode):
				k = k.encode("utf-8")
		if isinstance(v, unicode):
				v = v.encode("utf-8")
		tmp[k] = v
	headers = tmp

	if isinstance(body, unicode):
		body = body.encode("utf-8")

	msg = http_response(body, code, status, headers)
	m_raw = header + " " + msg
	sock.send(m_raw)

def inspect_worker():
	sock = ctx.socket(zmq.REP)
	sock.connect(config.get("handler", "proxy_inspect_spec"))

	while True:
		m_raw = sock.recv()
		m = tnetstring.loads(m_raw)
		print "IN: %s" % m

		# reply saying to always proxy
		id = m["id"]
		m = dict()
		m["id"] = id
		m["no-proxy"] = False

		print "OUT: %s" % m
		m_raw = tnetstring.dumps(m)
		sock.send(m_raw)

	sock.close()

def accept_worker():
	sock = ctx.socket(zmq.PULL)
	sock.connect(config.get("handler", "proxy_accept_in_spec"))

	while True:
		m_raw = sock.recv()
		m = tnetstring.loads(m_raw)
		print "IN: %s" % m
		sender = m["rids"][0]["sender"]
		id = m["rids"][0]["id"]
		instruct = json.loads(m["response"]["body"])
		hold = instruct["hold"]
		channel = hold["channels"][0]["name"]
		lock.acquire()
		hchannel = channels.get(channel)
		if not hchannel:
			hchannel = set()
			channels[channel] = hchannel
		hchannel.add((sender, id))
		lock.release()

	sock.close()

def push_in_zmq_worker():
	in_sock = ctx.socket(zmq.PULL)
	in_sock.bind(config.get("handler", "push_in_spec"))

	out_sock = ctx.socket(zmq.PUSH)
	out_sock.connect("inproc://push_in")

	while True:
		m_raw = in_sock.recv()
		try:
			try:
				m = tnetstring.loads(m_raw)
			except:
				raise ValidationError("bad format (not a tnetstring)")

			m = validate_publish(m)

		except ValidationError as e:
			print "warning: %s, dropping" % e

		out_sock.send(tnetstring.dumps(m))

	out_sock.linger = 0
	out_sock.close()

# return None for success or string on error
def push_in_http_handler(context, channel, m):
	out_sock = context["out_sock"]

	try:
		m = validate_http_publish(m)
	except ValidationError as e:
		return e.message

	for n, i in enumerate(m["items"]):
		out = dict()

		out["channel"] = ensure_utf8(channel)

		id = i.get("id")
		if id is not None:
			out["id"] = ensure_utf8(id)

		prev_id = i.get("prev-id")
		if prev_id is not None:
			out["prev-id"] = ensure_utf8(prev_id)

		for transport in ("http-response", "http-stream"):
			if transport in i:
				out[transport] = convert_json_transport(i[transport])

		out_sock.send(tnetstring.dumps(out))

def push_in_http_worker():
	out_sock = ctx.socket(zmq.PUSH)
	out_sock.connect("inproc://push_in")

	context = dict()
	context["out_sock"] = out_sock
	httpinterface.run(int(config.get("handler", "push_in_http_port")), push_in_http_handler, context)

	out_sock.linger = 0
	out_sock.close()

def push_in_worker(c):
	in_sock = ctx.socket(zmq.PULL)
	in_sock.bind("inproc://push_in")
	c.acquire()
	c.notify()
	c.release()

	out_sock = ctx.socket(zmq.PUB)
	for spec in config.get("handler", "m2_out_specs").split(","):
		out_sock.connect(spec)

	while True:
		m_raw = in_sock.recv()
		m = tnetstring.loads(m_raw)
		print "IN: %s" % m
		channel = m["channel"]
		rids = set()
		lock.acquire()
		hchannel = channels.get(channel)
		if hchannel:
			rids = hchannel
			del channels[channel]
		lock.release()
		body = m["http-response"]["body"]
		print "relaying to %d subscribers" % len(rids)
		for rid in rids:
			reply_http(out_sock, rid, 200, "OK", {}, body)

	in_sock.close()

inspect_thread = threading.Thread(target=inspect_worker)
inspect_thread.start()

accept_thread = threading.Thread(target=accept_worker)
accept_thread.start()

# we use a condition here to ensure the inproc bind succeeds before progressing
c = threading.Condition()
c.acquire()
push_in_thread = threading.Thread(target=push_in_worker, args=(c,))
push_in_thread.start()
c.wait()
c.release()

push_in_zmq_thread = threading.Thread(target=push_in_zmq_worker)
push_in_zmq_thread.start()

push_in_http_thread = threading.Thread(target=push_in_http_worker)
push_in_http_thread.start()

try:
	while True:
		time.sleep(60)
except KeyboardInterrupt:
	pass

httpinterface.stop()
ctx.term()

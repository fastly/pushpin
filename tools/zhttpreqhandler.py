# use route target zhttpreq/ipc:///tmp/zhttpreqhandler

import zmq
import tnetstring

zmq_context = zmq.Context()
sock = zmq_context.socket(zmq.REP)
sock.connect('ipc:///tmp/zhttpreqhandler')

while True:
	req = tnetstring.loads(sock.recv()[1:])

	resp = {
		'id': req['id'],
		'code': 200,
		'reason': 'OK',
		'headers': [
			['Content-Type', 'text/plain']
		],
		'body': 'hello there\n'
	}

	sock.send('T' + tnetstring.dumps(resp))

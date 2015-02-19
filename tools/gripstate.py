import sys
import tnetstring
import zmq

ctx = zmq.Context()
sock = ctx.socket(zmq.REP)
sock.connect(sys.argv[1])

class Rule(object):
	def __init__(self):
		self.domain = None
		self.path_prefix = None
		self.sid_ptr = None
		self.json_param = None

class Session(object):
	def __init__(self):
		self.last_ids = dict()

rules = list()
sessions = dict()

def session_detect_rules_set(new_rules):
	for nr in new_rules:
		found = False
		for r in rules:
			if r.domain == nr.domain and r.path_prefix == nr.path_prefix and r.sid_ptr == nr.sid_ptr and r.json_param == nr.json_param:
				found = True
				break
		if found:
			continue

		rules.append(nr)

def session_detect_rules_get(domain, path):
	out = list()
	for r in rules:
		if r.domain == domain and path.startswith(r.path_prefix):
			out.append(r)
	return out

def session_create_or_update(sid, last_ids):
	s = sessions.get(sid)
	if s is None:
		s = Session()
		sessions[sid] = s
	for k, v in last_ids.iteritems():
		s.last_ids[k] = v

def session_update(sid, last_ids):
	s = sessions.get(sid)
	if s is None:
		raise ValueError('unknown sid')
	for k, v in last_ids.iteritems():
		s.last_ids[k] = v

def session_get_last_ids(sid):
	s = sessions.get(sid)
	if s is not None:
		return s.last_ids
	else:
		return None

while True:
	req = tnetstring.loads(sock.recv())
	method = req['method']
	args = req['args']
	print 'IN %s %s' % (method, args)
	try:
		resp = None
		ret = None
		if method == 'session-detect-rules-set':
			rule_data_list = args['rules']
			rlist = list()
			for rule_data in rule_data_list:
				r = Rule()
				r.domain = rule_data['domain']
				r.path_prefix = rule_data['path-prefix']
				r.sid_ptr = rule_data['sid-ptr']
				r.json_param = rule_data.get('json-param')
				rlist.append(r)
			session_detect_rules_set(rlist)
		elif method == 'session-detect-rules-get':
			rlist = session_detect_rules_get(args['domain'], args['path'])
			ret = list()
			for r in rlist:
				i = {'domain': r.domain, 'path-prefix': r.path_prefix, 'sid-ptr': r.sid_ptr}
				if r.json_param:
					i['json-param'] = r.json_param
				ret.append(i)
		elif method == 'session-create-or-update':
			session_create_or_update(args['sid'], args['last-ids'])
		elif method == 'session-update-many':
			sid_last_ids = args['sid-last-ids']
			for sid, last_ids in sid_last_ids.iteritems():
				session_update(sid, last_ids)
		elif method == 'session-get-last-ids':
			ret = session_get_last_ids(args['sid'])
			if ret is None:
				resp = {'id': req['id'], 'success': False, 'condition': 'item-not-found'}
		else:
			resp = {'id': req['id'], 'success': False, 'condition': 'method-not-found'}

		if resp is None:
			resp = {'id': req['id'], 'success': True, 'value': ret}
	except:
		resp = {'id': req['id'], 'success': False, 'condition': 'general'}

	if resp['success']:
		print 'OUT %s' % resp['value']
	else:
		print 'OUT error, condition=%s' % resp['condition']
	sock.send(tnetstring.dumps(resp))

# Copyright (C) 2014 Fanout, Inc.
#
# This file is part of Pushpin.
#
# Pushpin is free software: you can redistribute it and/or modify it under
# the terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
# more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

import os
import time
import uuid
import traceback
import tnetstring
import zmq

zmq_context = zmq.Context()

# this converts any unicode to utf8 in a data structure tree
def ensure_utf8(i):
	if isinstance(i, dict):
		out = dict()
		for k, v in i.iteritems():
			out[ensure_utf8(k)] = ensure_utf8(v)
		return out
	elif isinstance(i, list):
		out = list()
		for v in i:
			out.append(ensure_utf8(v))
		return out
	elif isinstance(i, unicode):
		return i.encode("utf-8")
	else:
		return i

class CallError(Exception):
	def __init__(self, condition, message=None):
		super(CallError, self).__init__(message)
		self.condition = condition
		self.message = message

	def __str__(self):
		return "condition=%s, message=%s" % (self.condition, self.message)

class RpcClient(object):
	def __init__(self, req_sock_specs):
		self.specs = req_sock_specs
		self.sock = None
		self._reset_socket()

	def _reset_socket(self):
		if self.sock is not None:
			self.sock.linger = 0
			self.sock.close()
		self.sock = zmq_context.socket(zmq.DEALER)
		for spec in self.specs:
			self.sock.connect(spec)

	def call(self, method, args):
		req = dict()
		req['id'] = str(uuid.uuid4())
		req['method'] = ensure_utf8(method)
		req['args'] = ensure_utf8(args)
		req_raw = tnetstring.dumps(req)
		try:
			if not self.sock.poll(30000, zmq.POLLOUT):
				raise CallError('send-timeout')
			m_list = list()
			m_list.append('')
			m_list.append(req_raw)
			self.sock.send_multipart(m_list)
		except zmq.ZMQError as e:
			raise CallError('send-failed', e.message)
		start = int(time.clock() * 1000)
		while True:
			elapsed = max(int(time.clock() * 1000) - start, 0)
			try:
				if not self.sock.poll(max(30000 - elapsed, 0), zmq.POLLIN):
					raise CallError('receive-timeout')
				m_list = self.sock.recv_multipart()
			except zmq.ZMQError as e:
				raise CallError('receive-failed', e.message)
			if len(m_list) != 2:
				print "response has wrong number of parts, skipping"
				continue
			if len(m_list[0]) > 0:
				print "response first part is not empty, skipping"
				continue
			resp_raw = m_list[1]
			try:
				resp = tnetstring.loads(resp_raw)
			except:
				print "failed to parse response as tnetstring, skipping"
				continue
			if 'id' not in resp:
				print "response missing id field, skipping"
				continue
			if resp['id'] != req['id']:
				print "unexpected response id, skipping"
				continue
			break
		if 'success' not in resp:
			raise CallError('invalid-response', 'missing success field')
		if resp['success']:
			if 'value' not in resp:
				raise CallError('invalid-response', 'missing value field')
			return resp['value']
		else:
			if 'condition' not in resp:
				raise CallError('invalid-response', 'missing condition field')
			raise CallError(resp['condition'])

class RpcServer(object):
	def __init__(self, rep_sock_spec, context=None):
		if context:
			self.context = context
		else:
			self.context = zmq_context
		self.control_spec = 'inproc://pyobj-' + str(id(self))
		self.control_sock = self.context.socket(zmq.PAIR)
		self.control_sock.bind(self.control_spec)
		self.rep_sock = self.context.socket(zmq.REP)
		self.rep_sock.bind(rep_sock_spec)
		self.req_id = None
		if rep_sock_spec.startswith('ipc://'):
			os.chmod(rep_sock_spec[6:], 0o777)

	def _respond(self, value):
		resp = dict()
		if self.req_id:
			resp['id'] = self.req_id
		resp['success'] = True
		resp['value'] = ensure_utf8(value)
		self.rep_sock.send(tnetstring.dumps(resp))

	def _respond_error(self, condition):
		resp = dict()
		if self.req_id:
			resp['id'] = self.req_id
		resp['success'] = False
		resp['condition'] = ensure_utf8(condition)
		self.rep_sock.send(tnetstring.dumps(resp))

	def run(self, handler, data):
		poller = zmq.Poller()
		poller.register(self.control_sock, zmq.POLLIN)
		poller.register(self.rep_sock, zmq.POLLIN)

		while True:
			socks = dict(poller.poll())
			if socks.get(self.control_sock) == zmq.POLLIN:
				s = self.control_sock.recv()
				if s == 'stop':
					break
			elif socks.get(self.rep_sock) == zmq.POLLIN:
				req_raw = self.rep_sock.recv()
				try:
					req = tnetstring.loads(req_raw)
				except:
					self._respond_error('bad-request')
					continue

				self.req_id = req.get('id')

				method = req.get('method')
				if not method:
					self._respond_error('bad-request')
					continue

				args = req.get('args')
				if args is None:
					args = dict()
				if not isinstance(args, dict):
					self._respond_error('bad-request')
					continue

				try:
					ret = handler(method, args, data=data)
					self._respond(ret)
				except CallError as e:
					self._respond_error(e.condition)
				except:
					traceback.print_exc()
					self._respond_error('internal-server-error')

		self.rep_sock.linger = 0
		self.rep_sock.close()
		self.control_sock.send('finished')
		self.control_sock.close()

	def stop(self):
		sock = self.context.socket(zmq.PAIR)
		sock.connect(self.control_spec)
		sock.send('stop')
		sock.recv()

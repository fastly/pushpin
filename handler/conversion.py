# Copyright (C) 2012-2014 Fanout, Inc.
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

from base64 import b64decode

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

# convert json-style transport to tnetstring-style
def convert_json_transport(ttype, t):
	out = dict()
	if "code" in t:
		out["code"] = t["code"]
	if "reason" in t:
		out["reason"] = ensure_utf8(t["reason"])
	if "headers" in t:
		headers = list()
		if isinstance(t["headers"], list):
			for i in t["headers"]:
				headers.append([ensure_utf8(i[0]), ensure_utf8(i[1])])
		else:
			for k, v in t["headers"].iteritems():
				headers.append([ensure_utf8(k), ensure_utf8(v)])
		out["headers"] = headers
	if "body-bin" in t:
		out["body"] = ensure_utf8(b64decode(t["body-bin"]))
	elif "body" in t:
		out["body"] = ensure_utf8(t["body"])
	elif "body-patch" in t:
		out["body-patch"] = ensure_utf8(t["body-patch"])
	if "action" in t:
		out["action"] = ensure_utf8(t["action"])

	if ttype == "ws-message":
		# for ws-message, don't rename content-bin to content
		if "content-bin" in t:
			out["content-bin"] = ensure_utf8(b64decode(t["content-bin"]))
		elif "content" in t:
			out["content"] = ensure_utf8(t["content"])
	else:
		if "content-bin" in t:
			out["content"] = ensure_utf8(b64decode(t["content-bin"]))
		elif "content" in t:
			out["content"] = ensure_utf8(t["content"])

	return out

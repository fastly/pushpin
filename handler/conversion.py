# Copyright (C) 2012-2013 Fan Out Networks, Inc.
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

def ensure_utf8(s):
	if isinstance(s, unicode):
		return s.encode("utf-8")
	else:
		return s # assume it is already utf-8

# convert json-style transport to tnetstring-style
def convert_json_transport(t):
	out = dict()
	if "headers" in t:
		headers = dict()
		for k, v in t["headers"].iteritems():
			headers[ensure_utf8(k)] = ensure_utf8(v)
		out["headers"] = headers
	if "body" in t:
		out["body"] = ensure_utf8(t["body"])
	if "content" in t:
		out["content"] = ensure_utf8(t["content"])
	return out

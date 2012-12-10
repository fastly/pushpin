def ensure_utf8(s):
	if isinstance(s, unicode):
		return s.encode("utf-8")
	else:
		return s # assume it is already utf-8

# convert json-style transport to tnetstring-style
def convert_json_transport(t):
	out = dict()
	if "body" in t:
		out["body"] = ensure_utf8(t["body"])
	if "content" in t:
		out["content"] = ensure_utf8(t["content"])
	return out

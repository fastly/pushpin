/*
 * Copyright (C) 2015 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include "jsonpatch.h"

#include <assert.h>
#include "qtcompat.h"
#include "variant.h"
#include "jsonpointer.h"

namespace JsonPatch {

static void setSuccess(bool *ok, QString *errorMessage)
{
	if(ok)
		*ok = true;
	if(errorMessage)
		errorMessage->clear();
}

static void setError(bool *ok, QString *errorMessage, const QString &msg)
{
	if(ok)
		*ok = false;
	if(errorMessage)
		*errorMessage = msg;
}

static bool isKeyedObject(const Variant &in)
{
	return (typeId(in) == VariantType::Hash || typeId(in) == VariantType::Map);
}

static bool keyedObjectContains(const Variant &in, const QString &name)
{
	if(typeId(in) == VariantType::Hash)
		return in.toHash().contains(name);
	else if(typeId(in) == VariantType::Map)
		return in.toMap().contains(name);
	else
		return false;
}

static Variant keyedObjectGetValue(const Variant &in, const QString &name)
{
	if(typeId(in) == VariantType::Hash)
		return in.toHash().value(name);
	else if(typeId(in) == VariantType::Map)
		return in.toMap().value(name);
	else
		return Variant();
}

static Variant getChild(const Variant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	if(!isKeyedObject(in))
	{
		QString pn = !parentName.isEmpty() ? parentName : QString("value");
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return Variant();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	Variant v;
	if(typeId(in) == VariantType::Hash)
	{
		VariantHash h = in.toHash();

		if(!h.contains(childName))
		{
			if(required)
				setError(ok, errorMessage, QString("%1 does not contain '%2'").arg(pn, childName));
			else
				setSuccess(ok, errorMessage);

			return Variant();
		}

		v = h[childName];
	}
	else // Map
	{
		VariantMap m = in.toMap();

		if(!m.contains(childName))
		{
			if(required)
				setError(ok, errorMessage, QString("%1 does not contain '%2'").arg(pn, childName));
			else
				setSuccess(ok, errorMessage);

			return Variant();
		}

		v = m[childName];
	}

	setSuccess(ok, errorMessage);
	return v;
}

static QString getString(const Variant &in, bool *ok = 0)
{
	if(typeId(in) == VariantType::String)
	{
		if(ok)
			*ok = true;
		return in.toString();
	}
	else if(typeId(in) == VariantType::ByteArray)
	{
		if(ok)
			*ok = true;
		return QString::fromUtf8(in.toByteArray());
	}
	else
	{
		if(ok)
			*ok = false;
		return QString();
	}
}

static QString getString(const Variant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	bool ok_;
	Variant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return QString();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return QString();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	QString str = getString(v, &ok_);
	if(!ok_)
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn, childName));
		return QString();
	}

	setSuccess(ok, errorMessage);
	return str;
}

// Return true if item modified
static bool convertToJsonStyleInPlace(Variant *in)
{
	// Hash -> Map
	// ByteArray (UTF-8) -> String

	bool changed = false;

	VariantType::Type type = typeId(*in);
	if(type == VariantType::Hash)
	{
		VariantMap vmap;
		VariantHash vhash = in->toHash();
		QHashIterator<QString, Variant> it(vhash);
		while(it.hasNext())
		{
			it.next();
			Variant i = it.value();
			convertToJsonStyleInPlace(&i);
			vmap[it.key()] = i;
		}

		*in = vmap;
		changed = true;
	}
	else if(type == VariantType::List)
	{
		VariantList vlist = in->toList();
		for(int n = 0; n < vlist.count(); ++n)
		{
			Variant i = vlist.at(n);
			convertToJsonStyleInPlace(&i);
			vlist[n] = i;
		}

		*in = vlist;
		changed = true;
	}
	else if(type == VariantType::ByteArray)
	{
		*in = Variant(QString::fromUtf8(in->toByteArray()));
		changed = true;
	}

	return changed;
}

static Variant convertToJsonStyle(const Variant &in)
{
	Variant v = in;
	convertToJsonStyleInPlace(&v);
	return v;
}

static bool _compareJsonValues(const Variant &a, const Variant &b)
{
	if(typeId(a) == VariantType::Map && typeId(b) == VariantType::Map)
	{
		VariantMap am = a.toMap();
		VariantMap bm = b.toMap();

		if(am.count() != bm.count())
			return false;

		QMapIterator<QString, Variant> it(am);
		while(it.hasNext())
		{
			it.next();
			const QString &key = it.key();
			const Variant &val = it.value();

			if(!bm.contains(key))
				return false;
			if(!_compareJsonValues(val, bm.value(key)))
				return false;
		}

		return true;
	}
	else if(typeId(a) == VariantType::List && typeId(b) == VariantType::List)
	{
		VariantList al = a.toList();
		VariantList bl = b.toList();

		if(al.count() != bl.count())
			return false;

		for(int n = 0; n < al.count(); ++n)
		{
			if(!_compareJsonValues(al[n], bl[n]))
				return false;
		}

		return true;
	}
	else if(typeId(a) == VariantType::String && typeId(b) == VariantType::String)
	{
		return (a.toString() == b.toString());
	}
	else if(typeId(a) == VariantType::Bool && typeId(b) == VariantType::Bool)
	{
		return (a.toBool() == b.toBool());
	}
	else if(typeId(a) == VariantType::Invalid && typeId(b) == VariantType::Invalid)
	{
		return true;
	}
	else if(canConvert(a, VariantType::Int) && canConvert(b, VariantType::Int))
	{
		return (a.toInt() == b.toInt());
	}
	else
		return false;
}

static bool compareJsonValues(const Variant &a, const Variant &b)
{
	Variant ca = convertToJsonStyle(a);
	Variant cb = convertToJsonStyle(b);

	return _compareJsonValues(ca, cb);
}

Variant patch(const Variant &data, const VariantList &ops, QString *errorMessage)
{
	Variant out = data;

	foreach(const Variant &vop, ops)
	{
		if(!isKeyedObject(vop))
		{
			if(errorMessage)
				*errorMessage = "invalid op";
			return Variant();
		}

		QString pn = "op";

		bool ok;
		QString type = getString(vop, pn, "op", true, &ok, errorMessage);
		if(!ok)
			return Variant();

		QString path = getString(vop, pn, "path", true, &ok, errorMessage);
		if(!ok)
			return Variant();

		JsonPointer ptr;

		// For all ops except move, we can resolve the path now
		if(type != "move")
		{
			ptr = JsonPointer::resolve(&out, path, errorMessage);
			if(ptr.isNull())
				return Variant();
		}

		if(type == "add")
		{
			if(!keyedObjectContains(vop, "value"))
			{
				if(errorMessage)
					*errorMessage = "op does not contain 'value'";
				return Variant();
			}

			Variant value = keyedObjectGetValue(vop, "value");
			ptr.setValue(value);
		}
		else if(type == "remove")
		{
			if(!ptr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return Variant();
			}

			if(!ptr.remove())
				return Variant();
		}
		else if(type == "replace")
		{
			if(!keyedObjectContains(vop, "value"))
			{
				if(errorMessage)
					*errorMessage = "op does not contain 'value'";
				return Variant();
			}

			Variant value = keyedObjectGetValue(vop, "value");

			if(!ptr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return Variant();
			}

			ptr.setValue(value);
		}
		else if(type == "move")
		{
			QString from = getString(vop, pn, "from", true, &ok, errorMessage);
			if(!ok)
				return Variant();

			if(JsonPointer::isWithin(path, from))
			{
				if(errorMessage)
					*errorMessage = "cannot move location into itself";
				return Variant();
			}

			JsonPointer fromPtr = JsonPointer::resolve(&out, from, errorMessage);
			if(fromPtr.isNull())
				return Variant();

			if(!fromPtr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return Variant();
			}

			Variant value = fromPtr.take();

			ptr = JsonPointer::resolve(&out, path, errorMessage);
			if(ptr.isNull())
				return Variant();

			ptr.setValue(value);
		}
		else if(type == "copy")
		{
			QString from = getString(vop, pn, "from", true, &ok, errorMessage);
			if(!ok)
				return Variant();

			JsonPointer fromPtr = JsonPointer::resolve(&out, from, errorMessage);
			if(fromPtr.isNull())
				return Variant();

			if(!fromPtr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return Variant();
			}

			ptr = JsonPointer::resolve(&out, path, errorMessage);
			if(ptr.isNull())
				return Variant();

			ptr.setValue(fromPtr.value());
		}
		else if(type == "test")
		{
			if(!keyedObjectContains(vop, "value"))
			{
				if(errorMessage)
					*errorMessage = "op does not contain 'value'";
				return Variant();
			}

			Variant value = keyedObjectGetValue(vop, "value");
			Variant cur = ptr.value();

			if(!compareJsonValues(cur, value))
			{
				if(errorMessage)
					*errorMessage = "tested values are not equal";
				return Variant();
			}
		}
		else
		{
			if(errorMessage)
				*errorMessage = QString("unsupported op: %1").arg(type);
			return Variant();
		}
	}

	return out;
}

}

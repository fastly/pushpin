/*
 * Copyright (C) 2015 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include "jsonpatch.h"

#include <assert.h>
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

static bool isKeyedObject(const QVariant &in)
{
	return (in.type() == QVariant::Hash || in.type() == QVariant::Map);
}

static bool keyedObjectContains(const QVariant &in, const QString &name)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().contains(name);
	else if(in.type() == QVariant::Map)
		return in.toMap().contains(name);
	else
		return false;
}

static QVariant keyedObjectGetValue(const QVariant &in, const QString &name)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().value(name);
	else if(in.type() == QVariant::Map)
		return in.toMap().value(name);
	else
		return QVariant();
}

static QVariant getChild(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	if(!isKeyedObject(in))
	{
		QString pn = !parentName.isEmpty() ? parentName : QString("value");
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return QVariant();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	QVariant v;
	if(in.type() == QVariant::Hash)
	{
		QVariantHash h = in.toHash();

		if(!h.contains(childName))
		{
			if(required)
				setError(ok, errorMessage, QString("%1 does not contain '%2'").arg(pn, childName));
			else
				setSuccess(ok, errorMessage);

			return QVariant();
		}

		v = h[childName];
	}
	else // Map
	{
		QVariantMap m = in.toMap();

		if(!m.contains(childName))
		{
			if(required)
				setError(ok, errorMessage, QString("%1 does not contain '%2'").arg(pn, childName));
			else
				setSuccess(ok, errorMessage);

			return QVariant();
		}

		v = m[childName];
	}

	setSuccess(ok, errorMessage);
	return v;
}

static QString getString(const QVariant &in, bool *ok = 0)
{
	if(in.type() == QVariant::String)
	{
		if(ok)
			*ok = true;
		return in.toString();
	}
	else if(in.type() == QVariant::ByteArray)
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

static QString getString(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	bool ok_;
	QVariant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
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

// return true if item modified
static bool convertToJsonStyleInPlace(QVariant *in)
{
	// Hash -> Map
	// ByteArray (UTF-8) -> String

	bool changed = false;

	int type = in->type();
	if(type == QVariant::Hash)
	{
		QVariantMap vmap;
		QVariantHash vhash = in->toHash();
		QHashIterator<QString, QVariant> it(vhash);
		while(it.hasNext())
		{
			it.next();
			QVariant i = it.value();
			convertToJsonStyleInPlace(&i);
			vmap[it.key()] = i;
		}

		*in = vmap;
		changed = true;
	}
	else if(type == QVariant::List)
	{
		QVariantList vlist = in->toList();
		for(int n = 0; n < vlist.count(); ++n)
		{
			QVariant i = vlist.at(n);
			convertToJsonStyleInPlace(&i);
			vlist[n] = i;
		}

		*in = vlist;
		changed = true;
	}
	else if(type == QVariant::ByteArray)
	{
		*in = QVariant(QString::fromUtf8(in->toByteArray()));
		changed = true;
	}

	return changed;
}

static QVariant convertToJsonStyle(const QVariant &in)
{
	QVariant v = in;
	convertToJsonStyleInPlace(&v);
	return v;
}

static bool _compareJsonValues(const QVariant &a, const QVariant &b)
{
	if(a.type() == QVariant::Map && b.type() == QVariant::Map)
	{
		QVariantMap am = a.toMap();
		QVariantMap bm = b.toMap();

		if(am.count() != bm.count())
			return false;

		QMapIterator<QString, QVariant> it(am);
		while(it.hasNext())
		{
			it.next();
			const QString &key = it.key();
			const QVariant &val = it.value();

			if(!bm.contains(key))
				return false;
			if(!_compareJsonValues(val, bm.value(key)))
				return false;
		}

		return true;
	}
	else if(a.type() == QVariant::List && b.type() == QVariant::List)
	{
		QVariantList al = a.toList();
		QVariantList bl = b.toList();

		if(al.count() != bl.count())
			return false;

		for(int n = 0; n < al.count(); ++n)
		{
			if(!_compareJsonValues(al[n], bl[n]))
				return false;
		}

		return true;
	}
	else if(a.type() == QVariant::String && b.type() == QVariant::String)
	{
		return (a.toString() == b.toString());
	}
	else if(a.type() == QVariant::Bool && b.type() == QVariant::Bool)
	{
		return (a.toBool() == b.toBool());
	}
	else if(a.type() == QVariant::Invalid && b.type() == QVariant::Invalid)
	{
		return true;
	}
	else if(a.canConvert(QVariant::Int) && b.canConvert(QVariant::Int))
	{
		return (a.toInt() == b.toInt());
	}
	else
		return false;
}

static bool compareJsonValues(const QVariant &a, const QVariant &b)
{
	QVariant ca = convertToJsonStyle(a);
	QVariant cb = convertToJsonStyle(b);

	return _compareJsonValues(ca, cb);
}

QVariant patch(const QVariant &data, const QVariantList &ops, QString *errorMessage)
{
	QVariant out = data;

	foreach(const QVariant &vop, ops)
	{
		if(!isKeyedObject(vop))
		{
			if(errorMessage)
				*errorMessage = "invalid op";
			return QVariant();
		}

		QString pn = "op";

		bool ok;
		QString type = getString(vop, pn, "op", true, &ok, errorMessage);
		if(!ok)
			return QVariant();

		QString path = getString(vop, pn, "path", true, &ok, errorMessage);
		if(!ok)
			return QVariant();

		JsonPointer ptr;

		// for all ops except move, we can resolve the path now
		if(type != "move")
		{
			ptr = JsonPointer::resolve(&out, path, errorMessage);
			if(ptr.isNull())
				return QVariant();
		}

		if(type == "add")
		{
			if(!keyedObjectContains(vop, "value"))
			{
				if(errorMessage)
					*errorMessage = "op does not contain 'value'";
				return QVariant();
			}

			QVariant value = keyedObjectGetValue(vop, "value");
			ptr.setValue(value);
		}
		else if(type == "remove")
		{
			if(!ptr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return QVariant();
			}

			if(!ptr.remove())
				return QVariant();
		}
		else if(type == "replace")
		{
			if(!keyedObjectContains(vop, "value"))
			{
				if(errorMessage)
					*errorMessage = "op does not contain 'value'";
				return QVariant();
			}

			QVariant value = keyedObjectGetValue(vop, "value");

			if(!ptr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return QVariant();
			}

			ptr.setValue(value);
		}
		else if(type == "move")
		{
			QString from = getString(vop, pn, "from", true, &ok, errorMessage);
			if(!ok)
				return QVariant();

			if(JsonPointer::isWithin(path, from))
			{
				if(errorMessage)
					*errorMessage = "cannot move location into itself";
				return QVariant();
			}

			JsonPointer fromPtr = JsonPointer::resolve(&out, from, errorMessage);
			if(fromPtr.isNull())
				return QVariant();

			if(!fromPtr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return QVariant();
			}

			QVariant value = fromPtr.take();

			ptr = JsonPointer::resolve(&out, path, errorMessage);
			if(ptr.isNull())
				return QVariant();

			ptr.setValue(value);
		}
		else if(type == "copy")
		{
			QString from = getString(vop, pn, "from", true, &ok, errorMessage);
			if(!ok)
				return QVariant();

			JsonPointer fromPtr = JsonPointer::resolve(&out, from, errorMessage);
			if(fromPtr.isNull())
				return QVariant();

			if(!fromPtr.exists())
			{
				if(errorMessage)
					*errorMessage = "location does not exist";
				return QVariant();
			}

			ptr = JsonPointer::resolve(&out, path, errorMessage);
			if(ptr.isNull())
				return QVariant();

			ptr.setValue(fromPtr.value());
		}
		else if(type == "test")
		{
			if(!keyedObjectContains(vop, "value"))
			{
				if(errorMessage)
					*errorMessage = "op does not contain 'value'";
				return QVariant();
			}

			QVariant value = keyedObjectGetValue(vop, "value");
			QVariant cur = ptr.value();

			if(!compareJsonValues(cur, value))
			{
				if(errorMessage)
					*errorMessage = "tested values are not equal";
				return QVariant();
			}
		}
		else
		{
			if(errorMessage)
				*errorMessage = QString("unsupported op: %1").arg(type);
			return QVariant();
		}
	}

	return out;
}

}

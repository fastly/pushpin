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
	return (typeId(in) == QMetaType::QVariantHash || typeId(in) == QMetaType::QVariantMap);
}

static bool keyedObjectContains(const QVariant &in, const QString &name)
{
	if(typeId(in) == QMetaType::QVariantHash)
		return in.toHash().contains(name);
	else if(typeId(in) == QMetaType::QVariantMap)
		return in.toMap().contains(name);
	else
		return false;
}

static QVariant keyedObjectGetValue(const QVariant &in, const QString &name)
{
	if(typeId(in) == QMetaType::QVariantHash)
		return in.toHash().value(name);
	else if(typeId(in) == QMetaType::QVariantMap)
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
	if(typeId(in) == QMetaType::QVariantHash)
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
	if(typeId(in) == QMetaType::QString)
	{
		if(ok)
			*ok = true;
		return in.toString();
	}
	else if(typeId(in) == QMetaType::QByteArray)
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

// Return true if item modified
static bool convertToJsonStyleInPlace(QVariant *in)
{
	// Hash -> Map
	// ByteArray (UTF-8) -> String

	bool changed = false;

	QMetaType::Type type = typeId(*in);
	if(type == QMetaType::QVariantHash)
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
	else if(type == QMetaType::QVariantList)
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
	else if(type == QMetaType::QByteArray)
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
	if(typeId(a) == QMetaType::QVariantMap && typeId(b) == QMetaType::QVariantMap)
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
	else if(typeId(a) == QMetaType::QVariantList && typeId(b) == QMetaType::QVariantList)
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
	else if(typeId(a) == QMetaType::QString && typeId(b) == QMetaType::QString)
	{
		return (a.toString() == b.toString());
	}
	else if(typeId(a) == QMetaType::Bool && typeId(b) == QMetaType::Bool)
	{
		return (a.toBool() == b.toBool());
	}
	else if(typeId(a) == QMetaType::UnknownType && typeId(b) == QMetaType::UnknownType)
	{
		return true;
	}
	else if(canConvert(a, QMetaType::Int) && canConvert(b, QMetaType::Int))
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

		// For all ops except move, we can resolve the path now
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

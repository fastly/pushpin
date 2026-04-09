/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "variantutil.h"

#include "qtcompat.h"
#include "variant.h"

namespace VariantUtil {

void setSuccess(bool *ok, QString *errorMessage)
{
	if(ok)
		*ok = true;
	if(errorMessage)
		errorMessage->clear();
}

void setError(bool *ok, QString *errorMessage, const QString &msg)
{
	if(ok)
		*ok = false;
	if(errorMessage)
		*errorMessage = msg;
}

bool isKeyedObject(const Variant &in)
{
	return (typeId(in) == QMetaType::QVariantHash || typeId(in) == QMetaType::QVariantMap);
}

Variant createSameKeyedObject(const Variant &in)
{
	if(typeId(in) == QMetaType::QVariantHash)
		return VariantHash();
	else if(typeId(in) == QMetaType::QVariantMap)
		return VariantMap();
	else
		return Variant();
}

bool keyedObjectIsEmpty(const Variant &in)
{
	if(typeId(in) == QMetaType::QVariantHash)
		return in.toHash().isEmpty();
	else if(typeId(in) == QMetaType::QVariantMap)
		return in.toMap().isEmpty();
	else
		return true;
}

bool keyedObjectContains(const Variant &in, const QString &name)
{
	if(typeId(in) == QMetaType::QVariantHash)
		return in.toHash().contains(name);
	else if(typeId(in) == QMetaType::QVariantMap)
		return in.toMap().contains(name);
	else
		return false;
}

Variant keyedObjectGetValue(const Variant &in, const QString &name)
{
	if(typeId(in) == QMetaType::QVariantHash)
		return in.toHash().value(name);
	else if(typeId(in) == QMetaType::QVariantMap)
		return in.toMap().value(name);
	else
		return Variant();
}

void keyedObjectInsert(Variant *in, const QString &name, const Variant &value)
{
	if(typeId(*in) == QMetaType::QVariantHash)
	{
		VariantHash h = in->toHash();
		h.insert(name, value);
		*in = h;
	}
	else if(typeId(*in) == QMetaType::QVariantMap)
	{
		VariantMap h = in->toMap();
		h.insert(name, value);
		*in = h;
	}
}

Variant getChild(const Variant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
{
	if(!isKeyedObject(in))
	{
		QString pn = !parentName.isEmpty() ? parentName : QString("value");
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return Variant();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	Variant v;
	if(typeId(in) == QMetaType::QVariantHash)
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

Variant getKeyedObject(const Variant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
{
	bool ok_;
	Variant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return Variant();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return Variant();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	if(!isKeyedObject(v))
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn, childName));
		return Variant();
	}

	setSuccess(ok, errorMessage);
	return v;
}

VariantList getList(const Variant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
{
	bool ok_;
	Variant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return VariantList();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return VariantList();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	if(typeId(v) != QMetaType::QVariantList)
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn, childName));
		return VariantList();
	}

	setSuccess(ok, errorMessage);
	return v.toList();
}

QString getString(const Variant &in, bool *ok)
{
	if(typeId(in) == QMetaType::QString)
	{
		if(ok)
			*ok = true;
		return in.toString();
	}
	else if(typeId(in) == QMetaType::QByteArray)
	{
		QByteArray buf = in.toByteArray();
		if(ok)
			*ok = true;
		if(!buf.isNull())
			return QString::fromUtf8(buf);
		else
			return QString();
	}
	else
	{
		if(ok)
			*ok = false;
		return QString();
	}
}

QString getString(const Variant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
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

bool convertToJsonStyleInPlace(Variant *in)
{
	// Hash -> Map
	// ByteArray (UTF-8) -> String

	bool changed = false;

	QMetaType::Type type = typeId(*in);
	if(type == QMetaType::QVariantHash)
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
	else if(type == QMetaType::QVariantList)
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
	else if(type == QMetaType::QByteArray)
	{
		QByteArray buf = in->toByteArray();
		if(!buf.isNull())
			*in = QString::fromUtf8(buf);
		else
			*in = QString();
		changed = true;
	}

	return changed;
}

Variant convertToJsonStyle(const Variant &in)
{
	Variant v = in;
	convertToJsonStyleInPlace(&v);
	return v;
}

}

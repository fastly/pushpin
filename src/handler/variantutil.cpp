/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "variantutil.h"

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

bool isKeyedObject(const QVariant &in)
{
	return (in.type() == QVariant::Hash || in.type() == QVariant::Map);
}

QVariant createSameKeyedObject(const QVariant &in)
{
	if(in.type() == QVariant::Hash)
		return QVariantHash();
	else if(in.type() == QVariant::Map)
		return QVariantMap();
	else
		return QVariant();
}

bool keyedObjectIsEmpty(const QVariant &in)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().isEmpty();
	else if(in.type() == QVariant::Map)
		return in.toMap().isEmpty();
	else
		return true;
}

bool keyedObjectContains(const QVariant &in, const QString &name)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().contains(name);
	else if(in.type() == QVariant::Map)
		return in.toMap().contains(name);
	else
		return false;
}

QVariant keyedObjectGetValue(const QVariant &in, const QString &name)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().value(name);
	else if(in.type() == QVariant::Map)
		return in.toMap().value(name);
	else
		return QVariant();
}

void keyedObjectInsert(QVariant *in, const QString &name, const QVariant &value)
{
	if(in->type() == QVariant::Hash)
	{
		QVariantHash h = in->toHash();
		h.insert(name, value);
		*in = h;
	}
	else if(in->type() == QVariant::Map)
	{
		QVariantMap h = in->toMap();
		h.insert(name, value);
		*in = h;
	}
}

QVariant getChild(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
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

QVariant getKeyedObject(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
{
	bool ok_;
	QVariant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return QVariant();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return QVariant();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	if(!isKeyedObject(v))
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn, childName));
		return QVariant();
	}

	setSuccess(ok, errorMessage);
	return v;
}

QVariantList getList(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
{
	bool ok_;
	QVariant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return QVariantList();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return QVariantList();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	if(v.type() != QVariant::List)
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn, childName));
		return QVariantList();
	}

	setSuccess(ok, errorMessage);
	return v.toList();
}

QString getString(const QVariant &in, bool *ok)
{
	if(in.type() == QVariant::String)
	{
		if(ok)
			*ok = true;
		return in.toString();
	}
	else if(in.type() == QVariant::ByteArray)
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

QString getString(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok, QString *errorMessage)
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

bool convertToJsonStyleInPlace(QVariant *in)
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
		QByteArray buf = in->toByteArray();
		if(!buf.isNull())
			*in = QString::fromUtf8(buf);
		else
			*in = QString();
		changed = true;
	}

	return changed;
}

QVariant convertToJsonStyle(const QVariant &in)
{
	QVariant v = in;
	convertToJsonStyleInPlace(&v);
	return v;
}

}

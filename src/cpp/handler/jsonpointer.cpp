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

#include "jsonpointer.h"

#include <assert.h>
#include <QStringList>
#include "qtcompat.h"

JsonPointer::JsonPointer() :
	isNull_(true)
{
}

QVariant *JsonPointer::root()
{
	return root_;
}

JsonPointer::ExecStatus JsonPointer::execute(const QVariant *i, int refIndex, ConstFunc func, void *data) const
{
	// if there are more refs after current ref, step into current
	if(refIndex + 1 < refs_.count())
	{
		const Ref &ref = refs_[refIndex];

		assert(ref.type != Ref::Self);

		if(ref.type == Ref::Object)
		{
			if(typeId(*i) == QMetaType::QVariantHash)
			{
				QVariantHash h = i->toHash();
				if(!h.contains(ref.name))
					return ExecError;

				return execute(&h[ref.name], refIndex + 1, func, data);
			}
			else // Map
			{
				QVariantMap m = i->toMap();
				if(!m.contains(ref.name))
					return ExecError;

				return execute(&m[ref.name], refIndex + 1, func, data);
			}
		}
		else // Array
		{
			QVariantList l = i->toList();
			if(ref.index < 0 || ref.index >= l.count())
				return ExecError;

			return execute(&l[ref.index], refIndex + 1, func, data);
		}
	}

	// ensure ref is correct type
	const Ref &ref = refs_[refIndex];
	if(ref.type == Ref::Object && (typeId(*i) != QMetaType::QVariantHash && typeId(*i) != QMetaType::QVariantMap))
		return ExecError;
	else if(ref.type == Ref::Array && typeId(*i) != QMetaType::QVariantList)
		return ExecError;

	func(i, refs_[refIndex], data);
	return ExecContinue;
}

JsonPointer::ExecStatus JsonPointer::execute(QVariant *i, int refIndex, Func func, void *data)
{
	// if there are more refs after current ref, step into current
	if(refIndex + 1 < refs_.count())
	{
		const Ref &ref = refs_[refIndex];
		if(ref.type == Ref::Object)
		{
			if(typeId(*i) == QMetaType::QVariantHash)
			{
				QVariantHash h = i->toHash();
				if(!h.contains(ref.name))
					return ExecError;

				ExecStatus ret = execute(&h[ref.name], refIndex + 1, func, data);
				if(ret == ExecChanged)
					*i = h;
				return ret;
			}
			else // Map
			{
				QVariantMap m = i->toMap();
				if(!m.contains(ref.name))
					return ExecError;

				ExecStatus ret = execute(&m[ref.name], refIndex + 1, func, data);
				if(ret == ExecChanged)
					*i = m;
				return ret;
			}
		}
		else if(ref.type == Ref::Array)
		{
			QVariantList l = i->toList();
			if(ref.index < 0 || ref.index >= l.count())
				return ExecError;

			ExecStatus ret = execute(&l[ref.index], refIndex + 1, func, data);
			if(ret == ExecChanged)
				*i = l;
			return ret;
		}
	}

	// ensure ref is correct type
	const Ref &ref = refs_[refIndex];
	if(ref.type == Ref::Object && (typeId(*i) != QMetaType::QVariantHash && typeId(*i) != QMetaType::QVariantMap))
		return ExecError;
	else if(ref.type == Ref::Array && typeId(*i) != QMetaType::QVariantList)
		return ExecError;

	if(func(i, refs_[refIndex], data))
		return ExecChanged;
	else
		return ExecContinue;
}

bool JsonPointer::execute(ConstFunc func, void *data) const
{
	if(!refs_.isEmpty())
	{
		return (execute(root_, 0, func, data) != ExecError);
	}
	else
	{
		func(root_, Ref(), data);
		return true;
	}
}

bool JsonPointer::execute(Func func, void *data)
{
	if(!refs_.isEmpty())
	{
		return (execute(root_, 0, func, data) != ExecError);
	}
	else
	{
		func(root_, Ref(), data);
		return true;
	}
}

static void existsFunc(const QVariant *v, const JsonPointer::Ref &ref, void *data)
{
	QVariant &ret = *((QVariant *)data);

	if(ref.type == JsonPointer::Ref::Self)
	{
		ret = true;
	}
	else if(ref.type == JsonPointer::Ref::Object)
	{
		if(typeId(*v) == QMetaType::QVariantHash)
			ret = v->toHash().contains(ref.name);
		else // Map
			ret = v->toMap().contains(ref.name);
	}
	else // Array
	{
		QVariantList l = v->toList();
		ret = (ref.index >= 0 && ref.index < l.count());
	}
}

bool JsonPointer::exists() const
{
	QVariant ret;
	if(execute(existsFunc, &ret))
		return ret.toBool();
	else
		return false;
}

static void valueFunc(const QVariant *v, const JsonPointer::Ref &ref, void *data)
{
	QVariant &ret = *((QVariant *)data);

	if(ref.type == JsonPointer::Ref::Self)
	{
		ret = *v;
	}
	else if(ref.type == JsonPointer::Ref::Object)
	{
		if(typeId(*v) == QMetaType::QVariantHash)
			ret = v->toHash().value(ref.name);
		else // Map
			ret = v->toMap().value(ref.name);
	}
	else // Array
	{
		QVariantList l = v->toList();
		if(ref.index >= 0 && ref.index < l.count())
			ret = l[ref.index];
	}
}

QVariant JsonPointer::value() const
{
	QVariant ret;
	if(execute(valueFunc, &ret))
		return ret;
	else
		return QVariant();
}

static bool removeFunc(QVariant *v, const JsonPointer::Ref &ref, void *data)
{
	QVariant &ret = *((QVariant *)data);

	if(ref.type == JsonPointer::Ref::Self)
	{
		ret = false;
		return false; // technically an error, since we can't remove the root
	}
	else if(ref.type == JsonPointer::Ref::Object)
	{
		if(typeId(*v) == QMetaType::QVariantHash)
		{
			QVariantHash h = v->toHash();
			if(h.contains(ref.name))
			{
				ret = true;
				h.remove(ref.name);
				*v = h;
				return true;
			}
			else
				return false;
		}
		else // Map
		{
			QVariantMap m = v->toMap();
			if(m.contains(ref.name))
			{
				ret = true;
				m.remove(ref.name);
				*v = m;
				return true;
			}
			else
				return false;
		}
	}
	else // Array
	{
		QVariantList l = v->toList();
		if(ref.index >= 0 && ref.index < l.count())
		{
			ret = true;
			l.removeAt(ref.index);
			*v = l;
			return true;
		}
		else
			return false;
	}
}

bool JsonPointer::remove()
{
	QVariant ret;
	if(execute(removeFunc, &ret))
		return ret.toBool();
	else
		return false;
}

static bool takeFunc(QVariant *v, const JsonPointer::Ref &ref, void *data)
{
	QVariant &ret = *((QVariant *)data);

	if(ref.type == JsonPointer::Ref::Self)
	{
		ret = *v;
		return false; // technically an error, since we can't remove the root
	}
	else if(ref.type == JsonPointer::Ref::Object)
	{
		if(typeId(*v) == QMetaType::QVariantHash)
		{
			QVariantHash h = v->toHash();
			if(h.contains(ref.name))
			{
				ret = h.value(ref.name);
				h.remove(ref.name);
				*v = h;
				return true;
			}
			else
				return false;
		}
		else // Map
		{
			QVariantMap m = v->toMap();
			if(m.contains(ref.name))
			{
				ret = m.value(ref.name);
				m.remove(ref.name);
				*v = m;
				return true;
			}
			else
				return false;
		}
	}
	else // Array
	{
		QVariantList l = v->toList();
		if(ref.index >= 0 && ref.index < l.count())
		{
			ret = l[ref.index];
			l.removeAt(ref.index);
			*v = l;
			return true;
		}
		else
			return false;
	}
}

QVariant JsonPointer::take()
{
	QVariant ret;
	if(execute(takeFunc, &ret))
		return ret;
	else
		return QVariant();
}

static bool setValueFunc(QVariant *v, const JsonPointer::Ref &ref, void *_data)
{
	QPair<QVariant, QVariant> &data = *((QPair<QVariant, QVariant> *)_data);

	if(ref.type == JsonPointer::Ref::Self)
	{
		*v = data.first;
		data.second = true;
		return true;
	}
	else if(ref.type == JsonPointer::Ref::Object)
	{
		if(typeId(*v) == QMetaType::QVariantHash)
		{
			QVariantHash h = v->toHash();
			h[ref.name] = data.first;
			*v = h;
			data.second = true;
			return true;
		}
		else // Map
		{
			QVariantMap m = v->toMap();
			m[ref.name] = data.first;
			*v = m;
			data.second = true;
			return true;
		}
	}
	else // Array
	{
		QVariantList l = v->toList();
		if(ref.index == -1)
		{
			// append
			l += data.first;
			*v = l;
			data.second = true;
			return true;
		}
		else if(ref.index >= 0 && ref.index < l.count())
		{
			l[ref.index] = data.first;
			*v = l;
			data.second = true;
			return true;
		}
		else
			return false;
	}
}

bool JsonPointer::setValue(const QVariant &value)
{
	QPair<QVariant, QVariant> data;
	data.first = value;
	if(execute(setValueFunc, &data))
		return data.second.toBool();
	else
		return false;
}

bool JsonPointer::isWithin(const QString &bPointerStr, const QString &aPointerStr)
{
	if(!aPointerStr.startsWith('/') || !bPointerStr.startsWith('/') || aPointerStr == bPointerStr)
		return false;

	QStringList aParts = aPointerStr.split('/').mid(1);
	QStringList bParts = bPointerStr.split('/').mid(1);

	if(aParts.count() >= bParts.count())
		return false;

	for(int n = 0; n < aParts.count(); ++n)
	{
		if(aParts[n] != bParts[n])
			return false;
	}

	return true;
}

JsonPointer JsonPointer::resolve(QVariant *data, const QString &pointerStr, QString *errorMessage)
{
	if(!pointerStr.startsWith('/'))
	{
		if(errorMessage)
			*errorMessage = "pointer must start with /";
		return JsonPointer();
	}

	JsonPointer ptr;
	ptr.isNull_ = false;
	ptr.root_ = data;

	// root
	if(pointerStr.length() == 1)
		return ptr;

	QVariant i = *ptr.root_;
	QStringList parts = pointerStr.split('/').mid(1);
	foreach(const QString &part, parts)
	{
		if(part.isEmpty())
		{
			if(errorMessage)
				*errorMessage = "reference cannot be empty";
			return JsonPointer();
		}

		QString p = part;
		p.replace("~1", "/");
		p.replace("~0", "~");

		// validate and step into previous reference, if any
		if(!ptr.refs_.isEmpty())
		{
			const Ref &prevRef = ptr.refs_[ptr.refs_.count() - 1];

			if(prevRef.type == Ref::Object)
			{
				assert(typeId(i) == QMetaType::QVariantHash || typeId(i) == QMetaType::QVariantMap);

				if(typeId(i) == QMetaType::QVariantHash)
				{
					QVariantHash h = i.toHash();
					if(!h.contains(prevRef.name))
					{
						if(errorMessage)
							*errorMessage = QString("cannot step into undefined reference: key=%1").arg(prevRef.name);
						return JsonPointer();
					}

					i = h[prevRef.name];
				}
				else // Map
				{
					QVariantMap m = i.toMap();
					if(!m.contains(prevRef.name))
					{
						if(errorMessage)
							*errorMessage = QString("cannot step into undefined reference: key=%1").arg(prevRef.name);
						return JsonPointer();
					}

					i = m[prevRef.name];
				}
			}
			else // Array
			{
				QVariantList l = i.toList();
				if(prevRef.index < 0 || prevRef.index >= l.count())
				{
					if(errorMessage)
						*errorMessage = QString("cannot step into undefined reference: index=%1").arg(prevRef.index);
					return JsonPointer();
				}

				i = l[prevRef.index];
			}
		}

		if(typeId(i) == QMetaType::QVariantHash || typeId(i) == QMetaType::QVariantMap)
		{
			ptr.refs_ += Ref(p);
		}
		else if(typeId(i) == QMetaType::QVariantList)
		{
			QVariantList l = i.toList();
			if(p == "-")
			{
				ptr.refs_ += Ref(-1);
			}
			else
			{
				bool ok;
				int index = p.toInt(&ok);
				if(!ok)
				{
					if(errorMessage)
						*errorMessage = "index must be an integer";
					return JsonPointer();
				}

				if(index < 0 || index >= l.count())
				{
					if(errorMessage)
						*errorMessage = "index out of range";
					return JsonPointer();
				}

				ptr.refs_ += Ref(index);
			}
		}
		else
		{
			if(errorMessage)
				*errorMessage = "non-container value cannot have child reference";
			return JsonPointer();
		}
	}

	return ptr;
}

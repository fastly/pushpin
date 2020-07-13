/*
 * Copyright (C) 2015-2020 Fanout, Inc.
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

#ifndef JSONPOINTER_H
#define JSONPOINTER_H

#include <QString>
#include <QVariant>
#include <QVarLengthArray>

class JsonPointer
{
public:
	class Ref
	{
	public:
		enum Type
		{
			Self, // used for root
			Object,
			Array
		};

		Type type;
		QString name;
		int index;

		Ref() :
			type(Self),
			index(-1)
		{
		}

		Ref(const QString &_name) :
			type(Object),
			name(_name),
			index(-1)
		{
		}

		Ref(int _index) :
			type(Array),
			index(_index)
		{
		}
	};

	typedef void (*ConstFunc)(const QVariant *v, const Ref &ref, void *data);

	// return true if data was modified
	typedef bool (*Func)(QVariant *v, const Ref &ref, void *data);

	JsonPointer();

	inline bool isNull() const { return isNull_; }

	QVariant *root();
	bool execute(ConstFunc func, void *data) const;
	bool execute(Func func, void *data);
	bool exists() const;
	QVariant value() const;
	QVariant take();
	bool remove();
	bool setValue(const QVariant &value);

	static bool isWithin(const QString &bPointerStr, const QString &aPointerStr);
	static JsonPointer resolve(QVariant *data, const QString &pointerStr, QString *errorMessage = 0);

private:
	enum ExecStatus
	{
		ExecError,
		ExecContinue,
		ExecChanged
	};

	bool isNull_;
	QVariant *root_;
	QVarLengthArray<Ref, 16> refs_;

	ExecStatus execute(const QVariant *i, int refIndex, ConstFunc func, void *data) const;
	ExecStatus execute(QVariant *i, int refIndex, Func func, void *data);
};

#endif

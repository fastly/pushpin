/*
 * Copyright (C) 2015-2020 Fanout, Inc.
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
			Self, // Used for root
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

	// Return true if data was modified
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

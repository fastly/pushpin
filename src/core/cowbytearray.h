/*
 * Copyright (C) 2025 Fastly, Inc.
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
 */

#ifndef COWBYTEARRAY_H
#define COWBYTEARRAY_H

#include <string.h>
#include <QByteArray>
#include <QList>
#include "rust/bindings.h"

class CowByteArrayConstRef
{
public:
	CowByteArrayConstRef(const QByteArray &a) : inner_(a) {}

	bool isEmpty() const { return inner_.isEmpty(); }
	qsizetype size() const { return inner_.size(); }
	const char *data() const { return inner_.data(); }

	const QByteArray & asQByteArray() const { return inner_; }

private:
	friend class CowByteArray;

	const QByteArray &inner_;
};

class CowByteArrayRef
{
public:
	CowByteArrayRef(QByteArray &a) : inner_(a) {}

	bool isEmpty() const { return inner_.isEmpty(); }
	qsizetype size() const { return inner_.size(); }
	const char *data() const { return inner_.data(); }
	char *data() { return inner_.data(); }

	void resize(qsizetype size) { inner_.resize(size); }

	const QByteArray & asQByteArray() const { return inner_; }
	QByteArray & asQByteArray() { return inner_; }

private:
	friend class CowByteArray;

	QByteArray &inner_;
};

// QByteArray-like class that currently forwards to an inner QByteArray, to
// assist with reducing direct dependency on Qt. The API is designed to allow
// cheap conversion to/from QByteArray.
class CowByteArray
{
public:
	CowByteArray() : inner_(ffi::cow_byte_array_create()) {}
	CowByteArray(const CowByteArray &other) : inner_(ffi::cow_byte_array_copy(other.inner_)) {}
	CowByteArray(CowByteArrayConstRef ref) : CowByteArray(ref.inner_) {}
	CowByteArray(CowByteArrayRef ref) : CowByteArray(ref.inner_) {}

	CowByteArray(const char *data, qsizetype size = -1) :
		inner_(ffi::cow_byte_array_create())
	{
		if(size < 0)
			size = qstrlen(data);
		ffi::cow_byte_array_resize(&inner_, size);
		memcpy((void *)ffi::cow_byte_array_data_mut(&inner_), (void *)data, size);
	}

	CowByteArray(qsizetype size, char ch) :
		inner_(ffi::cow_byte_array_create())
	{
		ffi::cow_byte_array_resize(&inner_, size);
		memset((void *)ffi::cow_byte_array_data_mut(&inner_), ch, size);
	}

	CowByteArray(const QByteArray &other) :
		inner_(ffi::cow_byte_array_create())
	{
		ffi::cow_byte_array_resize(&inner_, other.size());
		memcpy((void *)ffi::cow_byte_array_data_mut(&inner_), (void *)other.data(), other.size());
	}

	~CowByteArray()
	{
		ffi::cow_byte_array_destroy(inner_);
	}

	CowByteArray &operator=(const CowByteArray &other)
	{
		ffi::cow_byte_array_destroy(inner_);
		inner_ = ffi::cow_byte_array_copy(other.inner_);
		return *this;
	}

	bool isEmpty() const { return ffi::cow_byte_array_size(inner_) == 0; }
	qsizetype size() const { return ffi::cow_byte_array_size(inner_); }
	const char *data() const{ return (const char *)ffi::cow_byte_array_data(inner_); }
	char *data() { q_.clear(); return (char *)ffi::cow_byte_array_data_mut(&inner_); }

	void resize(qsizetype size) { q_.clear(); ffi::cow_byte_array_resize(&inner_, size); }

	const QByteArray & asQByteArray() const { updateQ(); return q_; }
	QByteArray & asQByteArray() { updateQ(); return q_; }

private:
	const ffi::CowByteArray *inner_;
	mutable QByteArray q_;

	void updateQ() const
	{
		q_ = QByteArray((const char *)ffi::cow_byte_array_data(inner_), ffi::cow_byte_array_size(inner_));
	}
};

// QList-like class for working with CowByteArray that currently forwards to
// an inner QList<QByteArray>, to assist with reducing direct dependency on
// Qt. The API is designed to allow cheap conversion to/from
// QList<QByteArray> and to allow cheap conversions of inserts/lookups
// to/from QByteArray.
class CowByteArrayList
{
public:
	CowByteArrayList() = default;
	CowByteArrayList(const QList<QByteArray> &other) : inner_(other) {}

	bool isEmpty() const { return inner_.isEmpty(); }
	qsizetype count() const { return inner_.count(); }

	CowByteArrayList & operator+=(const CowByteArray &a) { inner_ += a.asQByteArray(); return *this; }
	CowByteArrayList & operator+=(const QByteArray &a) { inner_ += a; return *this; }

	CowByteArrayConstRef operator[](int index) const { return CowByteArrayConstRef(inner_[index]); }
	CowByteArrayRef operator[](int index) { return CowByteArrayRef(inner_[index]); }

	const QList<QByteArray> & asQByteArrayList() const { return inner_; }
	QList<QByteArray> & asQByteArrayList() { return inner_; }

private:
	QList<QByteArray> inner_;
};

#endif

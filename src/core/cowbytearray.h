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

#include <QByteArray>
#include <QList>

class CowByteArray;

class CowByteArrayConstRef
{
public:
	CowByteArrayConstRef(const QByteArray &a) : inner_(a) {}

	bool isEmpty() const { return inner_.isEmpty(); }
	ssize_t size() const { return inner_.size(); }
	ssize_t length() const { return size(); }
	const char *data() const { return inner_.data(); }

	ssize_t indexOf(char ch, ssize_t from = 0) const { return inner_.indexOf(ch, from); }
	CowByteArray mid(ssize_t pos, ssize_t len = -1) const;

	char operator[](ssize_t i) const { return inner_[(qsizetype)i]; }

	const QByteArray & asQByteArray() const { return inner_; }

private:
	friend class CowByteArray;

	const QByteArray &inner_;
};

class CowByteArrayRef
{
public:
	CowByteArrayRef(QByteArray &a) : inner_(a) {}
	CowByteArrayRef & operator=(const CowByteArray &other);

	bool isEmpty() const { return inner_.isEmpty(); }
	ssize_t size() const { return inner_.size(); }
	ssize_t length() const { return size(); }
	const char *data() const { return inner_.data(); }
	char *data() { return inner_.data(); }

	ssize_t indexOf(char ch, ssize_t from = 0) const { return inner_.indexOf(ch, from); }
	CowByteArray mid(ssize_t pos, ssize_t len = -1) const;

	void resize(ssize_t size) { inner_.resize(size); }

	char operator[](ssize_t i) const { return inner_[(qsizetype)i]; }
	char & operator[](ssize_t i) { return inner_[(qsizetype)i]; }

	const QByteArray & asQByteArray() const { return inner_; }

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
	CowByteArray() = default;
	CowByteArray(const CowByteArray &other): inner_(other.inner_) {}
	CowByteArray(CowByteArrayConstRef ref): inner_(ref.inner_) {}
	CowByteArray(CowByteArrayRef ref): inner_(ref.inner_) {}
	CowByteArray(const char *data, ssize_t size = -1) : inner_(data, size) {}
	CowByteArray(ssize_t size, char ch) : inner_(size, ch) {}
	CowByteArray(const QByteArray &other) : inner_(other) {}
	CowByteArray & operator=(const CowByteArray &other) { inner_ = other.inner_; return *this; }

	bool isNull() const { return inner_.isNull(); }
	bool isEmpty() const { return inner_.isEmpty(); }
	ssize_t size() const { return inner_.size(); }
	ssize_t length() const { return size(); }
	const char *data() const { return inner_.data(); }
	char *data() { return inner_.data(); }

	ssize_t indexOf(char ch, ssize_t from = 0) const { return inner_.indexOf(ch, from); }
	CowByteArray mid(ssize_t pos, ssize_t len = -1) const { return inner_.mid(pos, len); }
	CowByteArray trimmed() const { return inner_.trimmed(); }

	void clear() { inner_.clear(); }
	void resize(ssize_t size) { inner_.resize(size); }

	char operator[](ssize_t i) const { return inner_[(qsizetype)i]; }
	char & operator[](ssize_t i) { return inner_[(qsizetype)i]; }
	CowByteArray & operator+=(const CowByteArray &other) { inner_ += other.inner_; return *this; }
	CowByteArray & operator+=(char ch) { inner_ += ch; return *this; }
	CowByteArray & operator+=(const char *str) { inner_ += str; return *this; }

	const QByteArray & asQByteArray() const { return inner_; }

	friend CowByteArray operator+(const CowByteArray &lhs, const CowByteArray &rhs);
	friend CowByteArray operator+(const CowByteArray &lhs, const char *rhs);
	friend CowByteArray operator+(const char *lhs, const CowByteArray &rhs);

	friend bool operator==(const CowByteArray &lhs, const CowByteArray &rhs);
	friend bool operator==(const CowByteArray &lhs, const char *const &rhs);
	friend bool operator==(const char *const &lhs, const CowByteArray &rhs);

private:
	friend class CowByteArrayRef;

	QByteArray inner_;
};

inline CowByteArray operator+(const CowByteArray &lhs, const CowByteArray &rhs) { return lhs.inner_ + rhs.inner_; }
inline CowByteArray operator+(const CowByteArray &lhs, const char *rhs) { return lhs.inner_ + rhs; }
inline CowByteArray operator+(const char *lhs, const CowByteArray &rhs) { return lhs + rhs.inner_; }

inline bool operator==(const CowByteArray &lhs, const CowByteArray &rhs) { return lhs.inner_ == rhs.inner_; }
inline bool operator==(const CowByteArray &lhs, const char *const &rhs) { return lhs.inner_ == rhs; }
inline bool operator==(const char *const &lhs, const CowByteArray &rhs) { return lhs == rhs.inner_; }
inline bool operator!=(const CowByteArray &lhs, const CowByteArray &rhs) { return !(lhs == rhs); }
inline bool operator!=(const CowByteArray &lhs, const char *const &rhs) { return !(lhs == rhs); }
inline bool operator!=(const char *const &lhs, const CowByteArray &rhs) { return !(lhs == rhs); }

inline CowByteArray CowByteArrayConstRef::mid(ssize_t pos, ssize_t len) const { return inner_.mid(pos, len); }

inline CowByteArrayRef & CowByteArrayRef::operator=(const CowByteArray &other) { inner_ = other.inner_; return *this; }
inline CowByteArray CowByteArrayRef::mid(ssize_t pos, ssize_t len) const { return inner_.mid(pos, len); }

// QList-like class for working with CowByteArray that currently forwards to
// an inner QList<QByteArray>, to assist with reducing direct dependency on
// Qt. The API is designed to allow cheap conversion to/from
// QList<QByteArray> and to allow cheap conversions of inserts/lookups
// to/from QByteArray.
class CowByteArrayList
{
public:
	class iterator
	{
	public:
		typedef std::random_access_iterator_tag iterator_category;
		typedef qptrdiff difference_type;
		typedef CowByteArray value_type;
		typedef CowByteArray *pointer;
		typedef CowByteArrayRef reference;

		iterator(QList<QByteArray>::iterator it) : inner_(it) {}

		inline CowByteArrayRef operator*() const { return CowByteArrayRef(*inner_); }

		inline bool operator==(const iterator &other) const noexcept { return inner_ == other.inner_; }
		inline bool operator!=(const iterator &other) const noexcept { return !(inner_ == other.inner_); }

		inline iterator &operator++() { inner_++; return *this; }

	private:
		QList<QByteArray>::iterator inner_;
	};

	class const_iterator
	{
	public:
		typedef std::random_access_iterator_tag iterator_category;
		typedef qptrdiff difference_type;
		typedef CowByteArray value_type;
		typedef const CowByteArray *pointer;
		typedef CowByteArrayConstRef reference;

		const_iterator(QList<QByteArray>::const_iterator it) : inner_(it) {}

		inline CowByteArrayConstRef operator*() const { return CowByteArrayConstRef(*inner_); }

		inline bool operator==(const const_iterator &other) const noexcept { return inner_ == other.inner_; }
		inline bool operator!=(const const_iterator &other) const noexcept { return !(inner_ == other.inner_); }

		inline const_iterator &operator++() { inner_++; return *this; }

	private:
		QList<QByteArray>::const_iterator inner_;
	};

	CowByteArrayList() = default;
	CowByteArrayList(const QList<QByteArray> &other) : inner_(other) {}

	bool isEmpty() const { return inner_.isEmpty(); }
	ssize_t count() const { return inner_.count(); }
	bool contains(const CowByteArray &a) const { return inner_.contains(a.asQByteArray()); }

	iterator begin() { return inner_.begin(); }
	const_iterator begin() const { return inner_.begin(); }
	iterator end() { return inner_.end(); }
	const_iterator end() const { return inner_.end(); }

	CowByteArrayList & operator+=(const CowByteArrayList &other) { inner_ += other.asQByteArrayList(); return *this; }
	CowByteArrayList & operator+=(const CowByteArray &a) { inner_ += a.asQByteArray(); return *this; }
	CowByteArrayList & operator+=(const QByteArray &a) { inner_ += a; return *this; }
	CowByteArrayList & operator+=(const char *str) { inner_ += str; return *this; }

	CowByteArrayConstRef operator[](ssize_t index) const { return CowByteArrayConstRef(inner_[index]); }
	CowByteArrayRef operator[](ssize_t index) { return CowByteArrayRef(inner_[index]); }

	const QList<QByteArray> & asQByteArrayList() const { return inner_; }
	QList<QByteArray> & asQByteArrayList() { return inner_; }

private:
	QList<QByteArray> inner_;
};

#endif

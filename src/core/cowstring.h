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

#ifndef COWSTRING_H
#define COWSTRING_H

#include <QString>
#include "cowbytearray.h"

// QString-like class that currently forwards to an inner QString, to
// assist with reducing direct dependency on Qt. The API is designed to allow
// cheap conversion to/from QString.
class CowString
{
public:
	CowString() = default;
	CowString(const CowString &other): inner_(other.inner_) {}
	CowString(const char *str) : inner_(str) {}
	CowString(const QString &other) : inner_(other) {}
	CowString & operator=(const CowString &other) { inner_ = other.inner_; return *this; }

	bool isEmpty() const { return inner_.isEmpty(); }

	void clear() { inner_.clear(); }

	CowByteArray toUtf8() const { return inner_.toUtf8(); }

	const QString & asQString() const { return inner_; }
	QString & asQString() { return inner_; }

	friend bool operator==(const CowString &lhs, const CowString &rhs);
	friend bool operator==(const CowString &lhs, const char *const &rhs);
	friend bool operator==(const char *const &lhs, const CowString &rhs);

private:
	QString inner_;
};

inline bool operator==(const CowString &lhs, const CowString &rhs) { return lhs.inner_ == rhs.inner_; }
inline bool operator==(const CowString &lhs, const char *const &rhs) { return lhs.inner_ == rhs; }
inline bool operator==(const char *const &lhs, const CowString &rhs) { return lhs == rhs.inner_; }
inline bool operator!=(const CowString &lhs, const CowString &rhs) { return !(lhs == rhs); }
inline bool operator!=(const CowString &lhs, const char *const &rhs) { return !(lhs == rhs); }
inline bool operator!=(const char *const &lhs, const CowString &rhs) { return !(lhs == rhs); }

#endif

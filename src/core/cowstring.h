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

#include "cowbytearray.h"
#include <QString>
#include <QStringList>
#include <stdarg.h>

class CowString;

class CowStringConstRef {
public:
    CowStringConstRef(const QString &s) : inner_(s) {}

    bool isEmpty() const { return inner_.isEmpty(); }
    bool startsWith(const char *str) const { return inner_.startsWith(str); }
    bool contains(const char *str) const { return inner_.contains(str); }
    CowString mid(int position, int n = -1) const;

    const QString &asQString() const { return inner_; }

private:
    friend class CowString;

    const QString &inner_;
};

class CowStringRef {
public:
    CowStringRef(QString &s) : inner_(s) {}
    CowStringRef &operator=(const CowString &other);

    bool isEmpty() const { return inner_.isEmpty(); }
    bool startsWith(const char *str) const { return inner_.startsWith(str); }
    bool contains(const char *str) const { return inner_.contains(str); }
    CowString mid(int position, int n = -1) const;

    const QString &asQString() const { return inner_; }
    QString &asQString() { return inner_; }

private:
    friend class CowString;

    QString &inner_;
};

// QString-like class that currently forwards to an inner QString, to assist with reducing direct
// dependency on Qt. The API is designed to allow cheap conversion to/from QString.
class CowString {
public:
    CowString() = default;
    CowString(const CowString &other) : inner_(other.inner_) {}
    CowString(CowStringConstRef ref) : inner_(ref.inner_) {}
    CowString(CowStringRef ref) : inner_(ref.inner_) {}
    CowString(const char *str) : inner_(str) {}
    CowString(const QString &other) : inner_(other) {}
    CowString(int count, char ch) : inner_(count, QLatin1Char(ch)) {}
    CowString &operator=(const CowString &other) {
        inner_ = other.inner_;
        return *this;
    }

    CowString &operator+=(const CowString &other) {
        inner_ += other.inner_;
        return *this;
    }
    CowString &operator+=(const char *str) {
        inner_ += str;
        return *this;
    }
    CowString &operator+=(char ch) {
        inner_ += QLatin1Char(ch);
        return *this;
    }

    bool isEmpty() const { return inner_.isEmpty(); }

    void clear() { inner_.clear(); }

    bool startsWith(const char *str) const { return inner_.startsWith(str); }
    bool contains(const char *str) const { return inner_.contains(str); }
    CowString mid(int position, int n = -1) const { return inner_.mid(position, n); }

    CowString arg(const CowString &a) const { return inner_.arg(a.inner_); }
    CowString arg(const CowString &a1, const CowString &a2) const {
        return inner_.arg(a1.inner_, a2.inner_);
    }

    static CowString number(int n, int base = 10) { return QString::number(n, base); }
    static CowString number(long long n, int base = 10) { return QString::number(n, base); }
    static CowString number(double n, char format = 'g', int precision = 6) {
        return QString::number(n, format, precision);
    }

    CowByteArray toUtf8() const { return inner_.toUtf8(); }

    static CowString fromUtf8(const CowByteArray &ba) {
        return QString::fromUtf8(ba.asQByteArray());
    }

    int size() const { return inner_.size(); }
    int length() const { return inner_.length(); }
    CowString trimmed() const { return inner_.trimmed(); }
    CowString toLower() const { return inner_.toLower(); }
    CowString toUpper() const { return inner_.toUpper(); }
    CowString left(int n) const { return inner_.left(n); }
    CowString right(int n) const { return inner_.right(n); }
    CowString &append(const CowString &s) {
        inner_.append(s.inner_);
        return *this;
    }
    int indexOf(const char *str, int from = 0) const { return inner_.indexOf(str, from); }
    int indexOf(char ch, int from = 0) const { return inner_.indexOf(ch, from); }
    bool endsWith(const char *str) const { return inner_.endsWith(str); }
    int toInt(bool *ok = nullptr, int base = 10) const { return inner_.toInt(ok, base); }
    static CowString vasprintf(const char *fmt, va_list ap) { return QString::vasprintf(fmt, ap); }
    static CowString asprintf(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        CowString result = QString::vasprintf(fmt, ap);
        va_end(ap);
        return result;
    }

    const QString &asQString() const { return inner_; }
    QString &asQString() { return inner_; }

    friend bool operator==(const CowString &lhs, const CowString &rhs);
    friend bool operator==(const CowString &lhs, const char *const &rhs);
    friend bool operator==(const char *const &lhs, const CowString &rhs);
    friend CowString operator+(const CowString &lhs, const CowString &rhs);
    friend bool operator<(const CowString &lhs, const CowString &rhs);

private:
    friend class CowStringRef;

    QString inner_;
};

inline bool operator==(const CowString &lhs, const CowString &rhs) {
    return lhs.inner_ == rhs.inner_;
}
inline bool operator==(const CowString &lhs, const char *const &rhs) { return lhs.inner_ == rhs; }
inline bool operator==(const char *const &lhs, const CowString &rhs) { return lhs == rhs.inner_; }
inline bool operator!=(const CowString &lhs, const CowString &rhs) { return !(lhs == rhs); }
inline bool operator!=(const CowString &lhs, const char *const &rhs) { return !(lhs == rhs); }
inline bool operator!=(const char *const &lhs, const CowString &rhs) { return !(lhs == rhs); }
inline CowString operator+(const CowString &lhs, const CowString &rhs) {
    return lhs.inner_ + rhs.inner_;
}
inline bool operator<(const CowString &lhs, const CowString &rhs) {
    return lhs.inner_ < rhs.inner_;
}
inline size_t qHash(const CowString &key, size_t seed = 0) noexcept {
    return qHash(key.asQString(), seed);
}

inline CowString CowStringConstRef::mid(int position, int n) const {
    return inner_.mid(position, n);
}

inline CowStringRef &CowStringRef::operator=(const CowString &other) {
    inner_ = other.inner_;
    return *this;
}

inline CowString CowStringRef::mid(int position, int n) const { return inner_.mid(position, n); }

#define cowPrintable(s) (s).toUtf8().constData()

// QStringList-like class for working with CowString that currently forwards to an inner
// QStringList, to assist with reducing direct dependency on Qt. The API is designed to allow cheap
// conversion to/from QStringList and to allow cheap conversions of inserts/lookups to/from QString.
class CowStringList {
public:
    class iterator {
    public:
        typedef std::random_access_iterator_tag iterator_category;
        typedef qptrdiff difference_type;
        typedef CowString value_type;
        typedef CowString *pointer;
        typedef CowStringRef reference;

        iterator(QStringList::iterator it) : inner_(it) {}

        inline CowStringRef operator*() const { return CowStringRef(*inner_); }

        inline bool operator==(const iterator &other) const noexcept {
            return inner_ == other.inner_;
        }

        inline bool operator!=(const iterator &other) const noexcept {
            return !(inner_ == other.inner_);
        }

        inline iterator &operator++() {
            inner_++;
            return *this;
        }

    private:
        QStringList::iterator inner_;
    };

    class const_iterator {
    public:
        typedef std::random_access_iterator_tag iterator_category;
        typedef qptrdiff difference_type;
        typedef CowString value_type;
        typedef const CowString *pointer;
        typedef CowStringConstRef reference;

        const_iterator(QStringList::const_iterator it) : inner_(it) {}

        inline CowStringConstRef operator*() const { return CowStringConstRef(*inner_); }

        inline bool operator==(const const_iterator &other) const noexcept {
            return inner_ == other.inner_;
        }

        inline bool operator!=(const const_iterator &other) const noexcept {
            return !(inner_ == other.inner_);
        }

        inline const_iterator &operator++() {
            inner_++;
            return *this;
        }

    private:
        QStringList::const_iterator inner_;
    };

    CowStringList() = default;
    CowStringList(const QStringList &other) : inner_(other) {}
    CowStringList(std::initializer_list<CowString> list) {
        for (const CowString &s : list)
            inner_ += s.asQString();
    }

    bool isEmpty() const { return inner_.isEmpty(); }
    ssize_t count() const { return inner_.count(); }
    bool contains(const CowString &s) const { return inner_.contains(s.asQString()); }

    iterator begin() { return inner_.begin(); }
    const_iterator begin() const { return inner_.begin(); }
    iterator end() { return inner_.end(); }
    const_iterator end() const { return inner_.end(); }

    CowStringList &operator+=(const CowStringList &other) {
        inner_ += other.asQStringList();
        return *this;
    }

    CowStringList &operator+=(const CowString &s) {
        inner_ += s.asQString();
        return *this;
    }

    CowStringList &operator+=(const QString &s) {
        inner_ += s;
        return *this;
    }

    CowStringList &operator+=(const char *str) {
        inner_ += QString(str);
        return *this;
    }

    CowStringConstRef operator[](ssize_t index) const { return CowStringConstRef(inner_[index]); }
    CowStringRef operator[](ssize_t index) { return CowStringRef(inner_[index]); }

    CowString join(const CowString &sep) const { return inner_.join(sep.asQString()); }

    const QStringList &asQStringList() const { return inner_; }
    QStringList &asQStringList() { return inner_; }

private:
    QStringList inner_;
};

#endif

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

class CowByteArray
{
public:
    CowByteArray() = default;
    CowByteArray(CowByteArrayConstRef ref): inner_(ref.inner_) {}
    CowByteArray(CowByteArrayRef ref): inner_(ref.inner_) {}
    CowByteArray(const char *data, qsizetype size = -1) : inner_(data, size) {}
    CowByteArray(qsizetype size, char ch) : inner_(size, ch) {}
    CowByteArray(const QByteArray &other) : inner_(other) {}

    bool isEmpty() const { return inner_.isEmpty(); }
    qsizetype size() const { return inner_.size(); }
    const char *data() const { return inner_.data(); }
    char *data() { return inner_.data(); }

    void resize(qsizetype size) { inner_.resize(size); }

    const QByteArray & asQByteArray() const { return inner_; }
    QByteArray & asQByteArray() { return inner_; }

private:
    QByteArray inner_;
};

class CowByteArrayList
{
public:
    CowByteArrayList() = default;
    CowByteArrayList(const QList<QByteArray> &other) : inner_(other) {}

    bool isEmpty() const { return inner_.isEmpty(); }
    qsizetype count() const { return inner_.count(); }

    CowByteArrayList operator+=(const CowByteArray &a) { inner_ += a.asQByteArray(); return *this; }

    CowByteArrayConstRef operator[](int index) const { return CowByteArrayConstRef(inner_[index]); }
    CowByteArrayRef operator[](int index) { return CowByteArrayRef(inner_[index]); }

    const QList<QByteArray> & asQByteArrayList() const { return inner_; }
    QList<QByteArray> & asQByteArrayList() { return inner_; }

private:
    QList<QByteArray> inner_;
};

#endif

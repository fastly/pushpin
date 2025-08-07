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

#ifndef ACBYTEARRAY_H
#define ACBYTEARRAY_H

#include <QByteArray>
#include <QList>

class AcByteArrayConstRef
{
public:
    AcByteArrayConstRef(const QByteArray &a) : inner_(a) {}

    bool isEmpty() const { return inner_.isEmpty(); }
    qsizetype size() const { return inner_.size(); }
    const char *data() const { return inner_.data(); }

    const QByteArray & asQByteArray() const { return inner_; }

private:
    friend class AcByteArray;

    const QByteArray &inner_;
};

class AcByteArrayRef
{
public:
    AcByteArrayRef(QByteArray &a) : inner_(a) {}

    bool isEmpty() const { return inner_.isEmpty(); }
    qsizetype size() const { return inner_.size(); }
    const char *data() const { return inner_.data(); }
    char *data() { return inner_.data(); }

    void resize(qsizetype size) { inner_.resize(size); }

    const QByteArray & asQByteArray() const { return inner_; }
    QByteArray & asQByteArray() { return inner_; }

private:
    friend class AcByteArray;

    QByteArray &inner_;
};

class AcByteArray
{
public:
    AcByteArray() = default;
    AcByteArray(AcByteArrayConstRef ref): inner_(ref.inner_) {}
    AcByteArray(AcByteArrayRef ref): inner_(ref.inner_) {}
    AcByteArray(const char *data, qsizetype size = -1) : inner_(data, size) {}
    AcByteArray(qsizetype size, char ch) : inner_(size, ch) {}
    AcByteArray(const QByteArray &other) : inner_(other) {}

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

class AcByteArrayList
{
public:
    AcByteArrayList() = default;
    AcByteArrayList(const QList<QByteArray> &other) : inner_(other) {}

    bool isEmpty() const { return inner_.isEmpty(); }
    qsizetype count() const { return inner_.count(); }

    AcByteArrayList operator+=(const AcByteArray &a) { inner_ += a.asQByteArray(); return *this; }

    AcByteArrayConstRef operator[](int index) const { return AcByteArrayConstRef(inner_[index]); }
    AcByteArrayRef operator[](int index) { return AcByteArrayRef(inner_[index]); }

    const QList<QByteArray> & asQByteArrayList() const { return inner_; }
    QList<QByteArray> & asQByteArrayList() { return inner_; }

private:
    QList<QByteArray> inner_;
};

#endif

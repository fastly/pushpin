/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef TNETSTRING_H
#define TNETSTRING_H

#include <QVariant>

namespace TnetString {

enum Type
{
	ByteArray,
	Int,
	Double,
	Bool,
	Null,
	Hash,
	List
};

QByteArray fromByteArray(const QByteArray &in);
QByteArray fromInt(qint64 in);
QByteArray fromDouble(double in);
QByteArray fromBool(bool in);
QByteArray fromNull();
QByteArray fromHash(const QVariantHash &in);
QByteArray fromList(const QVariantList &in);
QByteArray fromVariant(const QVariant &in);

bool check(const QByteArray &in, int offset, Type *type, int *dataOffset, int *dataSize);
QByteArray toByteArray(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
qint64 toInt(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
double toDouble(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
bool toBool(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
void toNull(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
QVariantHash toHash(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
QVariantList toList(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
QVariant toVariant(const QByteArray &in, int offset, Type type, int dataOffset, int dataSize, bool *ok = 0);
QVariant toVariant(const QByteArray &in, int offset = 0, bool *ok = 0);

QString byteArrayToEscapedString(const QByteArray &in);

// pass >= 0 for pretty print, -1 for compact
QString variantToString(const QVariant &in, int indent = 0);

}

#endif

/*
 * Copyright (C) 2012-2013 Fanout, Inc.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
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
QByteArray fromInt(int in);
QByteArray fromDouble(double in);
QByteArray fromBool(bool in);
QByteArray fromNull();
QByteArray fromHash(const QVariantHash &in);
QByteArray fromList(const QVariantList &in);
QByteArray fromVariant(const QVariant &in);

bool check(const QByteArray &in, int offset, Type *type, int *dataOffset, int *dataSize);
QByteArray toByteArray(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
int toInt(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok = 0);
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

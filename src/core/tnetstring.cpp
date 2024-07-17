/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "tnetstring.h"

#include <assert.h>
#include "qtcompat.h"

namespace TnetString {

QByteArray fromByteArray(const QByteArray &in)
{
	return QByteArray::number(in.size()) + ':' + in + ',';
}

QByteArray fromInt(qint64 in)
{
	QByteArray val = QByteArray::number(in);
	return QByteArray::number(val.size()) + ':' + val + '#';
}

QByteArray fromDouble(double in)
{
	QByteArray val = QByteArray::number(in);
	return QByteArray::number(val.size()) + ':' + val + '^';
}

QByteArray fromBool(bool in)
{
	QByteArray val = in ? "true" : "false";
	return QByteArray::number(val.size()) + ':' + val + '!';
}

QByteArray fromNull()
{
	return QByteArray("0:~");
}

QByteArray fromVariant(const QVariant &in)
{
	switch(typeId(in))
	{
		case QMetaType::QByteArray:
			return fromByteArray(in.toByteArray());
		case QMetaType::Double:
			return fromDouble(in.toDouble());
		case QMetaType::Bool:
			return fromBool(in.toBool());
		case QMetaType::UnknownType:
			return fromNull();
		case QMetaType::QVariantHash:
			return fromHash(in.toHash());
		case QMetaType::QVariantList:
			return fromList(in.toList());
		default:
			if(canConvert(in, QMetaType::LongLong))
				return fromInt(in.toLongLong());

			// unsupported type
			assert(0);
			return QByteArray();
	}
}

QByteArray fromHash(const QVariantHash &in)
{
	QByteArray val;
	QHashIterator<QString, QVariant> it(in);
	while(it.hasNext())
	{
		it.next();
		val += fromByteArray(it.key().toUtf8());
		val += fromVariant(it.value());
	}
	return QByteArray::number(val.size()) + ':' + val + '}';
}

QByteArray fromList(const QVariantList &in)
{
	QByteArray val;
	foreach(const QVariant &v, in)
		val += fromVariant(v);
	return QByteArray::number(val.size()) + ':' + val + ']';
}

bool check(const QByteArray &in, int offset, Type *type, int *dataOffset, int *dataSize)
{
	int at = in.indexOf(':', offset);
	if(at == -1)
		return false;

	bool ok;
	int size = in.mid(offset, at - offset).toInt(&ok);
	if(!ok || size < 0)
		return false;

	char typeChar = in[at + 1 + size];
	Type type_;
	switch(typeChar)
	{
		case ',': type_ = ByteArray; break;
		case '#': type_ = Int; break;
		case '^': type_ = Double; break;
		case '!': type_ = Bool; break;
		case '~': type_ = Null; break;
		case '}': type_ = Hash; break;
		case ']': type_ = List; break;
		default: return false;
	}

	*type = type_;
	*dataOffset = at + 1;
	*dataSize = size;
	return true;
}

QByteArray toByteArray(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok)
{
	Q_UNUSED(offset);
	if(ok)
		*ok = true;
	return in.mid(dataOffset, dataSize);
}

qint64 toInt(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok)
{
	Q_UNUSED(offset);
	QByteArray val = in.mid(dataOffset, dataSize);
	bool ok_;
	qint64 x = val.toLongLong(&ok_);
	if(!ok_)
		x = 0;
	if(ok)
		*ok = ok_;
	return x;
}

double toDouble(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok)
{
	Q_UNUSED(offset);
	QByteArray val = in.mid(dataOffset, dataSize);
	bool ok_;
	double x = val.toDouble(&ok_);
	if(!ok_)
		x = 0;
	if(ok)
		*ok = ok_;
	return x;
}

bool toBool(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok)
{
	Q_UNUSED(offset);
	QByteArray val = in.mid(dataOffset, dataSize);
	if(val == "true")
	{
		if(ok)
			*ok = true;
		return true;
	}
	else if(val == "false")
	{
		if(ok)
			*ok = true;
		return false;
	}

	if(ok)
		*ok = false;
	return false;
}

void toNull(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok)
{
	Q_UNUSED(in);
	Q_UNUSED(offset);
	Q_UNUSED(dataOffset);
	Q_UNUSED(dataSize);
	*ok = true;
}

QVariant toVariant(const QByteArray &in, int offset, Type type, int dataOffset, int dataSize, bool *ok)
{
	QVariant val;
	bool ok_ = false;
	switch(type)
	{
		case ByteArray:
			val = toByteArray(in, offset, dataOffset, dataSize, &ok_);
			break;
		case Int:
			val = toInt(in, offset, dataOffset, dataSize, &ok_);
			break;
		case Double:
			val = toDouble(in, offset, dataOffset, dataSize, &ok_);
			break;
		case Bool:
			val = toBool(in, offset, dataOffset, dataSize, &ok_);
			break;
		case Null:
			toNull(in, offset, dataOffset, dataSize, &ok_);
			break;
		case Hash:
			val = toHash(in, offset, dataOffset, dataSize, &ok_);
			break;
		case List:
			val = toList(in, offset, dataOffset, dataSize, &ok_);
			break;
	}

	if(!ok_)
	{
		if(ok)
			*ok = false;
		return QVariant();
	}

	if(ok)
		*ok = true;
	return val;
}

QVariant toVariant(const QByteArray &in, int offset, bool *ok)
{
	Type type;
	int dataOffset;
	int dataSize;
	if(!check(in, offset, &type, &dataOffset, &dataSize))
	{
		if(ok)
			*ok = false;
		return QVariant();
	}

	return toVariant(in, offset, type, dataOffset, dataSize, ok);
}

QVariantHash toHash(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok)
{
	Q_UNUSED(offset);

	QVariantHash out;

	int at = dataOffset;
	while(at < dataSize + dataOffset)
	{
		Type itype;
		int ioffset;
		int isize;
		if(!check(in, at, &itype, &ioffset, &isize))
		{
			if(ok)
				*ok = false;
			return QVariantHash();
		}

		if(itype != ByteArray)
		{
			if(ok)
				*ok = false;
			return QVariantHash();
		}

		bool ok_;
		QByteArray key = toByteArray(in, at, ioffset, isize, &ok_);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return QVariantHash();
		}

		at = ioffset + isize + 1; // position to value

		if(!check(in, at, &itype, &ioffset, &isize))
		{
			if(ok)
				*ok = false;
			return QVariantHash();
		}

		QVariant val = toVariant(in, at, itype, ioffset, isize, &ok_);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return QVariantHash();
		}

		out[QString::fromUtf8(key)] = val;
		at = ioffset + isize + 1; // position to next item
	}

	if(ok)
		*ok = true;
	return out;
}

QVariantList toList(const QByteArray &in, int offset, int dataOffset, int dataSize, bool *ok)
{
	Q_UNUSED(offset);

	QVariantList out;

	int at = dataOffset;
	while(at < dataOffset + dataSize)
	{
		Type itype;
		int ioffset;
		int isize;
		if(!check(in, at, &itype, &ioffset, &isize))
		{
			if(ok)
				*ok = false;
			return QVariantList();
		}

		bool ok_;
		QVariant val = toVariant(in, at, itype, ioffset, isize, &ok_);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return QVariantList();
		}

		out += val;
		at = ioffset + isize + 1; // position to next item
	}

	if(ok)
		*ok = true;
	return out;
}

QString byteArrayToEscapedString(const QByteArray &in)
{
	QString out;
	for(int n = 0; n < in.size(); ++n)
	{
		char c = in[n];
		if(c == '\\')
			out += "\\\\";
		else if(c == '\"')
			out += "\\\"";
		else if(c == '\n')
			out += "\\n";
		else if(c >= 0x20 && c < 0x7f)
			out += QChar::fromLatin1(c);
		else
			out += QString::asprintf("\\x%02x", (unsigned char)c);
	}
	return out;
}

QString variantToString(const QVariant &in, int indent)
{
	QString out;

	QMetaType::Type type = typeId(in);
	if(type == QMetaType::QVariantHash)
	{
		QVariantHash hash = in.toHash();

		out += '{';
		if(indent >= 0)
			out += '\n';
		else
			out += ' ';

		QHashIterator<QString, QVariant> it(hash);
		while(it.hasNext())
		{
			it.next();

			if(indent >= 0)
				out += QString(indent + 2, ' ');

			out += '\"' + byteArrayToEscapedString(it.key().toUtf8()) + "\": " + variantToString(it.value(), indent >= 0 ? indent + 2 : -1);
			if(it.hasNext())
				out += ',';

			if(indent >= 0)
				out += '\n';
			else
				out += ' ';
		}

		if(indent >= 0)
			out += QString(indent, ' ');
		out += '}';
	}
	else if(type == QMetaType::QVariantList)
	{
		QVariantList list = in.toList();

		out += '[';
		if(indent >= 0)
			out += '\n';
		else
			out += ' ';

		for(int n = 0; n < list.count(); ++n)
		{
			if(indent >= 0)
				out += QString(indent + 2, ' ');

			out += variantToString(list[n], indent >= 0 ? indent + 2 : -1);
			if(n + 1 < list.count())
				out += ',';

			if(indent >= 0)
				out += '\n';
			else
				out += ' ';
		}

		if(indent >= 0)
			out += QString(indent, ' ');
		out += ']';
	}
	else if(type == QMetaType::QByteArray)
	{
		QByteArray val = in.toByteArray();
		out += '\"' + byteArrayToEscapedString(val) + '\"';
	}
	else if(type == QMetaType::Double)
		out += QString::number(in.toDouble());
	else if(type == QMetaType::Bool)
		out += in.toBool() ? "true" : "false";
	else if(type == QMetaType::UnknownType)
		out += "null";
	else if(canConvert(in, QMetaType::LongLong))
		out += QString::number(in.toLongLong());
	else
		out += QString("<unknown: %1>").arg((int)type);

	return out;
}

}

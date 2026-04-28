/*
 * Copyright (C) 2024 Fastly, Inc.
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

#ifndef QTCOMPAT_H
#define QTCOMPAT_H

#include <QMetaType>
#include <QVariant>

namespace VariantType {
	enum Type {
		Invalid = QMetaType::UnknownType,
		Bool = QMetaType::Bool,
		Int = QMetaType::Int,
		Double = QMetaType::Double,
		LongLong = QMetaType::LongLong,
		String = QMetaType::QString,
		ByteArray = QMetaType::QByteArray,
		Hash = QMetaType::QVariantHash,
		Map = QMetaType::QVariantMap,
		List = QMetaType::QVariantList
	};
}

inline VariantType::Type typeId(const QVariant &v)
{
#if QT_VERSION >= 0x060000
	return (VariantType::Type)v.typeId();
#else
	return (VariantType::Type)v.type();
#endif
}

inline bool canConvert(const QVariant &v, VariantType::Type type)
{
#if QT_VERSION >= 0x060000
    return v.canConvert(QMetaType((QMetaType::Type)type));
#else
    return v.canConvert((QMetaType::Type)type);
#endif
}

#endif

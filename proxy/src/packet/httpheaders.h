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

#ifndef HTTPHEADERS_H
#define HTTPHEADERS_H

#include <QByteArray>
#include <QPair>
#include <QList>

typedef QPair<QByteArray, QByteArray> HttpHeader;

class HttpHeaders : public QList<HttpHeader>
{
public:
	bool contains(const QByteArray &key) const;
	QByteArray get(const QByteArray &key) const;
	QList<QByteArray> getAll(const QByteArray &key, bool split = true) const;
	QList<QByteArray> takeAll(const QByteArray &key, bool split = true);
	void removeAll(const QByteArray &key);

	static QByteArray join(const QList<QByteArray> &values);
};

#endif

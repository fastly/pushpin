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

#include "httpheaders.h"

QByteArray HttpHeaders::get(const QByteArray &key) const
{
	for(int n = 0; n < count(); ++n)
	{
		const HttpHeader &h = at(n);
		if(qstricmp(h.first.data(), key.data()) == 0)
			return at(n).second;
	}

	return QByteArray();
}

bool HttpHeaders::contains(const QByteArray &key) const
{
	for(int n = 0; n < count(); ++n)
	{
		if(qstricmp(at(n).first.data(), key.data()) == 0)
			return true;
	}

	return false;
}

void HttpHeaders::removeAll(const QByteArray &key)
{
	for(int n = 0; n < count(); ++n)
	{
		if(qstricmp(at(n).first.data(), key.data()) == 0)
		{
			removeAt(n);
			--n; // adjust position
		}
	}
}

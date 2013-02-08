/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
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

#ifndef ZURLREQUESTPACKET_H
#define ZURLREQUESTPACKET_H

#include <QUrl>
#include <QVariant>
#include "httpheaders.h"

class ZurlRequestPacket
{
public:
	QByteArray id;
	QByteArray sender;
	int seq;

	bool cancel;
	QString method;
	QUrl uri;
	HttpHeaders headers;
	QByteArray body;
	bool more;
	bool stream;
	int maxSize;
	QString connectHost;
	bool ignorePolicies;
	int credits;
	QVariant userData;

	ZurlRequestPacket();

	QVariant toVariant() const;
	bool fromVariant(const QVariant &in);
};

#endif

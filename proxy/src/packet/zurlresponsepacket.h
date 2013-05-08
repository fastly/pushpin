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

#ifndef ZURLRESPONSEPACKET_H
#define ZURLRESPONSEPACKET_H

#include <QVariant>
#include "httpheaders.h"

class ZurlResponsePacket
{
public:
	QByteArray id;
	int seq;

	bool isError;
	QByteArray condition;

	QByteArray replyAddress;
	int code;
	QByteArray reason;
	HttpHeaders headers;
	QByteArray body;
	bool more;
	int credits;
	QVariant userData;

	ZurlResponsePacket();

	QVariant toVariant() const;
	bool fromVariant(const QVariant &in);
};

#endif

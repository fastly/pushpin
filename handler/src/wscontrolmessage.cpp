/*
 * Copyright (C) 2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "wscontrolmessage.h"

#include <QVariant>
#include "variantutil.h"

using namespace VariantUtil;

WsControlMessage WsControlMessage::fromVariant(const QVariant &in, bool *ok, QString *errorMessage)
{
	QString pn = "grip control packet";

	if(!isKeyedObject(in))
	{
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return WsControlMessage();
	}

	pn = "grip control object";

	WsControlMessage out;

	bool ok_;
	QString type = getString(in, pn, "type", true, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return WsControlMessage();
	}

	if(type == "subscribe")
		out.type = Subscribe;
	else if(type == "unsubscribe")
		out.type = Unsubscribe;
	else if(type == "detach")
		out.type = Detach;
	else if(type == "session")
		out.type = Session;
	else if(type == "set-meta")
		out.type = SetMeta;
	else
	{
		setError(ok, errorMessage, QString("'type' contains unknown value: %1").arg(type));
		return WsControlMessage();
	}

	if(out.type == Subscribe || out.type == Unsubscribe)
	{
		out.channel = getString(in, pn, "channel", true, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}

		if(out.channel.isEmpty())
		{
			setError(ok, errorMessage, QString("%1 contains 'channel' with invalid value").arg(pn));
			return WsControlMessage();
		}

		if(out.type == Subscribe)
		{
			QVariantList vfilters = getList(in, pn, "filters", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return WsControlMessage();
			}

			foreach(const QVariant &vfilter, vfilters)
			{
				QString filter = getString(vfilter, &ok_);
				if(!ok_)
				{
					setError(ok, errorMessage, "filters contains value with wrong type");
					return WsControlMessage();
				}

				out.filters += filter;
			}
		}
	}
	else if(out.type == Session)
	{
		out.sessionId = getString(in, pn, "id", true, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}

		if(out.sessionId.isEmpty())
		{
			setError(ok, errorMessage, QString("%1 contains 'id' with invalid value").arg(pn));
			return WsControlMessage();
		}
	}
	else if(out.type == SetMeta)
	{
		out.metaName = getString(in, pn, "name", true, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}

		if(out.metaName.isEmpty())
		{
			setError(ok, errorMessage, QString("%1 contains 'name' with invalid value").arg(pn));
			return WsControlMessage();
		}

		out.metaValue = getString(in, pn, "value", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}
	}

	return out;
}

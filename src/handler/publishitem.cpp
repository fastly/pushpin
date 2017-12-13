/*
 * Copyright (C) 2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include "publishitem.h"

#include "variantutil.h"

using namespace VariantUtil;

PublishItem PublishItem::fromVariant(const QVariant &vitem, const QString &channel, bool *ok, QString *errorMessage)
{
	QString pn = "publish item object";

	if(!isKeyedObject(vitem))
	{
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return PublishItem();
	}

	PublishItem item;
	bool ok_;

	if(!channel.isEmpty())
	{
		item.channel = channel;
	}
	else
	{
		item.channel = getString(vitem, pn, "channel", true, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}
	}

	item.id = getString(vitem, pn, "id", false, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return PublishItem();
	}

	item.prevId = getString(vitem, pn, "prev-id", false, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return PublishItem();
	}

	QVariant vformats = getKeyedObject(vitem, pn, "formats", false, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return PublishItem();
	}

	if(!vformats.isValid())
	{
		vformats = createSameKeyedObject(vitem);

		QVariant v = keyedObjectGetValue(vitem, "http-response");
		if(v.isValid())
			keyedObjectInsert(&vformats, "http-response", v);

		v = keyedObjectGetValue(vitem, "http-stream");
		if(v.isValid())
			keyedObjectInsert(&vformats, "http-stream", v);

		v = keyedObjectGetValue(vitem, "ws-message");
		if(v.isValid())
			keyedObjectInsert(&vformats, "ws-message", v);
	}

	if(keyedObjectIsEmpty(vformats))
	{
		setError(ok, errorMessage, "no formats specified");
		return PublishItem();
	}

	if(keyedObjectContains(vformats, "http-response"))
	{
		PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpResponse, keyedObjectGetValue(vformats, "http-response"), &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		item.formats.insert(f.type, f);
	}

	if(keyedObjectContains(vformats, "http-stream"))
	{
		PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpStream, keyedObjectGetValue(vformats, "http-stream"), &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		item.formats.insert(f.type, f);
	}

	if(keyedObjectContains(vformats, "ws-message"))
	{
		PublishFormat f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, keyedObjectGetValue(vformats, "ws-message"), &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		item.formats.insert(f.type, f);
	}

	QVariant vmeta = getKeyedObject(vitem, pn, "meta", false, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return PublishItem();
	}

	if(vmeta.isValid())
	{
		if(vmeta.type() == QVariant::Hash)
		{
			QVariantHash hmeta = vmeta.toHash();

			QHashIterator<QString, QVariant> it(hmeta);
			while(it.hasNext())
			{
				it.next();
				const QString &key = it.key();
				const QVariant &vval = it.value();

				QString val = getString(vval, &ok_);
				if(!ok_)
				{
					setError(ok, errorMessage, QString("'meta' contains '%1' with wrong type").arg(key));
					return PublishItem();
				}

				item.meta[key] = val;
			}
		}
		else // Map
		{
			QVariantMap mmeta = vmeta.toMap();

			QMapIterator<QString, QVariant> it(mmeta);
			while(it.hasNext())
			{
				it.next();
				const QString &key = it.key();
				const QVariant &vval = it.value();

				QString val = getString(vval, &ok_);
				if(!ok_)
				{
					setError(ok, errorMessage, QString("'meta' contains '%1' with wrong type").arg(key));
					return PublishItem();
				}

				item.meta[key] = val;
			}
		}
	}

	if(keyedObjectContains(vitem, "size"))
	{
		QVariant vsize = keyedObjectGetValue(vitem, "size");
		if(!vsize.canConvert(QVariant::Int))
		{
			setError(ok, errorMessage, QString("%1 contains 'size' with wrong type").arg(pn));
			return PublishItem();
		}

		item.size = vsize.toInt();

		if(item.size < 0)
		{
			setError(ok, errorMessage, QString("%1 contains 'size' with invalid value").arg(pn));
			return PublishItem();
		}
	}

	if(keyedObjectContains(vitem, "no-seq"))
	{
		QVariant vnoSeq = keyedObjectGetValue(vitem, "no-seq");
		if(vnoSeq.type() != QVariant::Bool)
		{
			setError(ok, errorMessage, QString("%1 contains 'no-seq' with wrong type").arg(pn));
			return PublishItem();
		}

		item.noSeq = vnoSeq.toBool();
	}

	setSuccess(ok, errorMessage);
	return item;
}

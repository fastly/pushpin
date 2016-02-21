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

#include "publishformat.h"

#include "variantutil.h"
#include "statusreasons.h"

using namespace VariantUtil;

PublishFormat PublishFormat::fromVariant(Type type, const QVariant &in, bool *ok, QString *errorMessage)
{
	QString pn;
	if(type == HttpResponse)
		pn = "'http-response'";
	else if(type == HttpStream)
		pn = "'http-stream'";
	else // WebSocketMessage
		pn = "'ws-message'";

	if(!isKeyedObject(in))
	{
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return PublishFormat();
	}

	PublishFormat out(type);
	bool ok_;

	if(type == HttpResponse)
	{
		if(keyedObjectContains(in, "code"))
		{
			QVariant vcode = keyedObjectGetValue(in, "code");
			if(!vcode.canConvert(QVariant::Int))
			{
				setError(ok, errorMessage, QString("%1 contains 'code' with wrong type").arg(pn));
				return PublishFormat();
			}

			out.code = vcode.toInt();

			if(out.code < 0 || out.code > 999)
			{
				setError(ok, errorMessage, QString("%1 contains 'code' with invalid value").arg(pn));
				return PublishFormat();
			}
		}
		else
			out.code = 200;

		QString reasonStr = getString(in, pn, "reason", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishFormat();
		}

		if(!reasonStr.isEmpty())
			out.reason = reasonStr.toUtf8();
		else
			out.reason = StatusReasons::getReason(out.code);

		if(keyedObjectContains(in, "headers"))
		{
			QVariant vheaders = keyedObjectGetValue(in, "headers");
			if(vheaders.type() == QVariant::List)
			{
				foreach(const QVariant &vheader, vheaders.toList())
				{
					if(vheader.type() != QVariant::List)
					{
						setError(ok, errorMessage, "headers contains element with wrong type");
						return PublishFormat();
					}

					QVariantList lheader = vheader.toList();
					if(lheader.count() != 2)
					{
						setError(ok, errorMessage, "headers contains list with wrong number of elements");
						return PublishFormat();
					}

					QString name = getString(lheader[0], &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, "header contains name element with wrong type");
						return PublishFormat();
					}

					QString val = getString(lheader[1], &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, "header contains value element with wrong type");
						return PublishFormat();
					}

					out.headers += HttpHeader(name.toUtf8(), val.toUtf8());
				}
			}
			else if(isKeyedObject(vheaders))
			{
				if(vheaders.type() == QVariant::Hash)
				{
					QVariantHash hheaders = vheaders.toHash();

					QHashIterator<QString, QVariant> it(hheaders);
					while(it.hasNext())
					{
						it.next();
						const QString &key = it.key();
						const QVariant &vval = it.value();

						QString val = getString(vval, &ok_);
						if(!ok_)
						{
							setError(ok, errorMessage, QString("headers contains '%1' with wrong type").arg(key));
							return PublishFormat();
						}

						out.headers += HttpHeader(key.toUtf8(), val.toUtf8());
					}
				}
				else // Map
				{
					QVariantMap mheaders = vheaders.toMap();

					QMapIterator<QString, QVariant> it(mheaders);
					while(it.hasNext())
					{
						it.next();
						const QString &key = it.key();
						const QVariant &vval = it.value();

						QString val = getString(vval, &ok_);
						if(!ok_)
						{
							setError(ok, errorMessage, QString("headers contains '%1' with wrong type").arg(key));
							return PublishFormat();
						}

						out.headers += HttpHeader(key.toUtf8(), val.toUtf8());
					}
				}
			}
			else
			{
				setError(ok, errorMessage, QString("%1 contains 'headers' with wrong type").arg(pn));
				return PublishFormat();
			}
		}

		if(in.type() == QVariant::Map && keyedObjectContains(in, "body-bin")) // JSON input
		{
			QString bodyBin = getString(in, pn, "body-bin", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return PublishFormat();
			}

			out.body = QByteArray::fromBase64(bodyBin.toUtf8());
		}
		else if(keyedObjectContains(in, "body"))
		{
			QVariant vcontent = keyedObjectGetValue(in, "body");
			if(vcontent.type() == QVariant::ByteArray)
				out.body = vcontent.toByteArray();
			else if(vcontent.type() == QVariant::String)
				out.body = vcontent.toString().toUtf8();
			else
			{
				setError(ok, errorMessage, QString("%1 contains 'body' with wrong type").arg(pn));
				return PublishFormat();
			}
		}
		else if(keyedObjectContains(in, "body-patch"))
		{
			out.bodyPatch = getList(in, pn, "body-patch", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return PublishFormat();
			}

			out.haveBodyPatch = true;
		}
		else
		{
			if(in.type() == QVariant::Map) // JSON input
				setError(ok, errorMessage, QString("%1 does not contain 'body', 'body-bin', or 'body-patch'").arg(pn));
			else
				setError(ok, errorMessage, QString("%1 does not contain 'body' or 'body-patch'").arg(pn));
			return PublishFormat();
		}
	}
	else if(type == HttpStream)
	{
		QString action = getString(in, pn, "action", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishFormat();
		}

		if(action == "close")
			out.close = true;

		if(!out.close)
		{
			if(in.type() == QVariant::Map && keyedObjectContains(in, "content-bin")) // JSON input
			{
				QString contentBin = getString(in, pn, "content-bin", false, &ok_, errorMessage);
				if(!ok_)
				{
					if(ok)
						*ok = false;
					return PublishFormat();
				}

				out.body = QByteArray::fromBase64(contentBin.toUtf8());
			}
			else if(keyedObjectContains(in, "content"))
			{
				QVariant vcontent = keyedObjectGetValue(in, "content");
				if(vcontent.type() == QVariant::ByteArray)
					out.body = vcontent.toByteArray();
				else if(vcontent.type() == QVariant::String)
					out.body = vcontent.toString().toUtf8();
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(pn));
					return PublishFormat();
				}
			}
			else
			{
				if(in.type() == QVariant::Map) // JSON input
					setError(ok, errorMessage, QString("%1 does not contain 'content' or 'content-bin'").arg(pn));
				else
					setError(ok, errorMessage, QString("%1 does not contain 'content'").arg(pn));
				return PublishFormat();
			}
		}
	}
	else if(type == WebSocketMessage)
	{
		if(keyedObjectContains(in, "content-bin"))
		{
			QVariant vcontentBin = keyedObjectGetValue(in, "content-bin");

			if(in.type() == QVariant::Map) // JSON input
			{
				if(vcontentBin.type() != QVariant::String)
				{
					setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
					return PublishFormat();
				}

				out.body = QByteArray::fromBase64(vcontentBin.toString().toUtf8());
			}
			else
			{
				if(vcontentBin.type() != QVariant::ByteArray)
				{
					setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
					return PublishFormat();
				}

				out.body = vcontentBin.toByteArray();
			}

			out.binary = true;
		}
		else if(keyedObjectContains(in, "content"))
		{
			QVariant vcontent = keyedObjectGetValue(in, "content");
			if(vcontent.type() == QVariant::ByteArray)
				out.body = vcontent.toByteArray();
			else if(vcontent.type() == QVariant::String)
				out.body = vcontent.toString().toUtf8();
			else
			{
				setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(pn));
				return PublishFormat();
			}
		}
		else
		{
			setError(ok, errorMessage, QString("%1 does not contain 'content' or 'content-bin'").arg(pn));
			return PublishFormat();
		}
	}

	if(ok)
		*ok = true;
	return out;
}

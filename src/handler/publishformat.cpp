/*
 * Copyright (C) 2016-2020 Fanout, Inc.
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

#include "publishformat.h"

#include "qtcompat.h"
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

	QString action = getString(in, pn, "action", false, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return PublishFormat();
	}

	if(action == "hint")
	{
		out.action = Hint;
	}
	else if(action == "close")
	{
		out.action = Close;
	}
	else if(action == "refresh")
	{
		out.action = Refresh;
	}
	else if(action.isNull() || action == "send") // Default
	{
		out.action = Send;
	}
	else
	{
		if(ok)
			*ok = false;
		return PublishFormat();
	}

	if(type == HttpResponse)
	{
		if(out.action == Send)
		{
			if(keyedObjectContains(in, "code"))
			{
				QVariant vcode = keyedObjectGetValue(in, "code");
				if(!canConvert(vcode, QMetaType::Int))
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
				if(typeId(vheaders) == QMetaType::QVariantList)
				{
					foreach(const QVariant &vheader, vheaders.toList())
					{
						if(typeId(vheader) != QMetaType::QVariantList)
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
					if(typeId(vheaders) == QMetaType::QVariantHash)
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

			if(keyedObjectContains(in, "content-filters"))
			{
				QVariant vfilters = keyedObjectGetValue(in, "content-filters");
				if(typeId(vfilters) != QMetaType::QVariantList)
				{
					setError(ok, errorMessage, QString("%1 contains 'content-filters' with wrong type").arg(pn));
					return PublishFormat();
				}

				QStringList filters;
				foreach(const QVariant &vfilter, vfilters.toList())
				{
					QString filter = getString(vfilter, &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, "content-filters contains element with wrong type");
						return PublishFormat();
					}

					filters += filter;
				}

				out.haveContentFilters = true;
				out.contentFilters = filters;
			}

			if(typeId(in) == QMetaType::QVariantMap && keyedObjectContains(in, "body-bin")) // JSON input
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
				if(typeId(vcontent) == QMetaType::QByteArray)
					out.body = vcontent.toByteArray();
				else if(typeId(vcontent) == QMetaType::QString)
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
				if(typeId(in) == QMetaType::QVariantMap) // JSON input
					setError(ok, errorMessage, QString("%1 does not contain 'body', 'body-bin', or 'body-patch'").arg(pn));
				else
					setError(ok, errorMessage, QString("%1 does not contain 'body' or 'body-patch'").arg(pn));
				return PublishFormat();
			}
		}
	}
	else if(type == HttpStream)
	{
		if(out.action == Send)
		{
			if(keyedObjectContains(in, "content-filters"))
			{
				QVariant vfilters = keyedObjectGetValue(in, "content-filters");
				if(typeId(vfilters) != QMetaType::QVariantList)
				{
					setError(ok, errorMessage, QString("%1 contains 'content-filters' with wrong type").arg(pn));
					return PublishFormat();
				}

				QStringList filters;
				foreach(const QVariant &vfilter, vfilters.toList())
				{
					QString filter = getString(vfilter, &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, "content-filters contains element with wrong type");
						return PublishFormat();
					}

					filters += filter;
				}

				out.haveContentFilters = true;
				out.contentFilters = filters;
			}

			if(typeId(in) == QMetaType::QVariantMap && keyedObjectContains(in, "content-bin")) // JSON input
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
				if(typeId(vcontent) == QMetaType::QByteArray)
					out.body = vcontent.toByteArray();
				else if(typeId(vcontent) == QMetaType::QString)
					out.body = vcontent.toString().toUtf8();
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(pn));
					return PublishFormat();
				}
			}
			else
			{
				if(typeId(in) == QMetaType::QVariantMap) // JSON input
					setError(ok, errorMessage, QString("%1 does not contain 'content' or 'content-bin'").arg(pn));
				else
					setError(ok, errorMessage, QString("%1 does not contain 'content'").arg(pn));
				return PublishFormat();
			}
		}
	}
	else if(type == WebSocketMessage)
	{
		if(out.action == Send)
		{
			QString typeStr = getString(in, pn, "type", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return PublishFormat();
			}

			if(!typeStr.isNull())
			{
				if(typeStr == "text")
					out.messageType = Text;
				else if(typeStr == "binary")
					out.messageType = Binary;
				else if(typeStr == "ping")
					out.messageType = Ping;
				else if(typeStr == "pong")
					out.messageType = Pong;
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'type' with unknown value").arg(pn));
					return PublishFormat();
				}
			}

			if(keyedObjectContains(in, "content-filters"))
			{
				QVariant vfilters = keyedObjectGetValue(in, "content-filters");
				if(typeId(vfilters) != QMetaType::QVariantList)
				{
					setError(ok, errorMessage, QString("%1 contains 'content-filters' with wrong type").arg(pn));
					return PublishFormat();
				}

				QStringList filters;
				foreach(const QVariant &vfilter, vfilters.toList())
				{
					QString filter = getString(vfilter, &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, "content-filters contains element with wrong type");
						return PublishFormat();
					}

					filters += filter;
				}

				out.haveContentFilters = true;
				out.contentFilters = filters;
			}

			if(keyedObjectContains(in, "content-bin"))
			{
				QVariant vcontentBin = keyedObjectGetValue(in, "content-bin");

				if(typeId(in) == QMetaType::QVariantMap) // JSON input
				{
					if(typeId(vcontentBin) != QMetaType::QString)
					{
						setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
						return PublishFormat();
					}

					out.body = QByteArray::fromBase64(vcontentBin.toString().toUtf8());
				}
				else
				{
					if(typeId(vcontentBin) != QMetaType::QByteArray)
					{
						setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
						return PublishFormat();
					}

					out.body = vcontentBin.toByteArray();
				}

				if(((int)out.messageType) == -1)
					out.messageType = Binary;
			}
			else if(keyedObjectContains(in, "content"))
			{
				QVariant vcontent = keyedObjectGetValue(in, "content");
				if(typeId(vcontent) == QMetaType::QByteArray)
					out.body = vcontent.toByteArray();
				else if(typeId(vcontent) == QMetaType::QString)
					out.body = vcontent.toString().toUtf8();
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(pn));
					return PublishFormat();
				}

				if(((int)out.messageType) == -1)
					out.messageType = Text;
			}
			else if(out.messageType == Text || out.messageType == Binary || ((int)out.messageType) == -1)
			{
				setError(ok, errorMessage, QString("%1 does not contain 'content' or 'content-bin'").arg(pn));
				return PublishFormat();
			}
		}
		else if(out.action == Close)
		{
			if(keyedObjectContains(in, "code"))
			{
				QVariant vcode = keyedObjectGetValue(in, "code");
				if(!canConvert(vcode, QMetaType::Int))
				{
					setError(ok, errorMessage, QString("%1 contains 'code' with wrong type").arg(pn));
					return PublishFormat();
				}

				out.code = vcode.toInt();

				if(out.code < 0)
				{
					setError(ok, errorMessage, QString("%1 contains 'code' with invalid value").arg(pn));
					return PublishFormat();
				}
			}

			QString reasonStr = getString(in, pn, "reason", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return PublishFormat();
			}

			if(!reasonStr.isEmpty())
				out.reason = reasonStr.toUtf8();
		}

	}

	if(ok)
		*ok = true;
	return out;
}

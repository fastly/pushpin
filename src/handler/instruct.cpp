/*
 * Copyright (C) 2016-2019 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#include "instruct.h"

#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include "qtcompat.h"
#include "variantutil.h"
#include "statusreasons.h"
#include "filter.h"

#define DEFAULT_RESPONSE_TIMEOUT 55
#define MINIMUM_RESPONSE_TIMEOUT 5
#define DEFAULT_NEXTLINK_TIMEOUT 120

using namespace VariantUtil;

static int charToHex(char c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	else if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	else if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	else
		return -1;
}
static QByteArray unescape(const QByteArray &in)
{
	QByteArray out;

	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '\\')
		{
			if(n + 1 >= in.length())
				return QByteArray();

			++n;

			if(in[n] == '\\')
			{
				out += '\\';
			}
			else if(in[n] == 'r')
			{
				out += '\r';
			}
			else if(in[n] == 'n')
			{
				out += '\n';
			}
			else if(in[n] == 'x')
			{
				if(n + 2 >= in.length())
					return QByteArray();

				int hi = charToHex(in[n + 1]);
				int lo = charToHex(in[n + 2]);
				n += 2;

				if(hi == -1 || lo == -1)
					return QByteArray();

				unsigned int x = (hi << 4) + lo;
				out += (char)x;
			}
		}
		else
			out += in[n];
	}

	return out;
}

Instruct Instruct::fromResponse(const HttpResponseData &response, bool *ok, QString *errorMessage)
{
	HoldMode holdMode = NoHold;
	QList<Channel> channels;
	int timeout = -1;
	QList<QByteArray> exposeHeaders;
	KeepAliveMode keepAliveMode = NoKeepAlive;
	QByteArray keepAliveData;
	int keepAliveTimeout = -1;
	QHash<QString, QString> meta;
	HttpResponseData newResponse;

	if(response.headers.contains("Grip-Hold"))
	{
		QByteArray gripHoldStr = response.headers.get("Grip-Hold").asQByteArray();
		if(gripHoldStr == "response")
		{
			holdMode = ResponseHold;
		}
		else if(gripHoldStr == "stream")
		{
			holdMode = StreamHold;
		}
		else
		{
			setError(ok, errorMessage, "Grip-Hold must be set to either 'response' or 'stream'");
			return Instruct();
		}
	}

	QList<HttpHeaderParameters> gripChannels = response.headers.getAllAsParameters("Grip-Channel");
	foreach(const HttpHeaderParameters &gripChannel, gripChannels)
	{
		if(gripChannel.isEmpty())
		{
			setError(ok, errorMessage, "failed to parse Grip-Channel");
			return Instruct();
		}

		Channel c;
		c.name = QString::fromUtf8(gripChannel[0].first.asQByteArray());
		QByteArray param = gripChannel.get("prev-id").asQByteArray();
		if(!param.isNull())
			c.prevId = QString::fromUtf8(param);

		for(int n = 1; n < gripChannel.count(); ++n)
		{
			const HttpHeaderParameter &param = gripChannel[n];
			if(param.first == "filter")
				c.filters += QString::fromUtf8(param.second.asQByteArray());
		}

		if(c.filters.count() > MESSAGEFILTERSTACK_SIZE_MAX)
		{
			setError(ok, errorMessage, QString("too many filters for channel '%1'").arg(c.name));
			return Instruct();
		}

		channels += c;
	}

	if(response.headers.contains("Grip-Timeout"))
	{
		bool x;
		timeout = response.headers.get("Grip-Timeout").asQByteArray().toInt(&x);
		if(!x)
		{
			setError(ok, errorMessage, "failed to parse Grip-Timeout");
			return Instruct();
		}

		if(timeout < 0)
		{
			setError(ok, errorMessage, "Grip-Timeout has invalid value");
			return Instruct();
		}
	}

	exposeHeaders = response.headers.getAll("Grip-Expose-Headers").asQByteArrayList();

	HttpHeaderParameters keepAliveParams = response.headers.getAsParameters("Grip-Keep-Alive");
	if(!keepAliveParams.isEmpty())
	{
		QByteArray val = keepAliveParams[0].first.asQByteArray();
		if(val.isEmpty())
		{
			setError(ok, errorMessage, "Grip-Keep-Alive cannot be empty");
			return Instruct();
		}

		QByteArray mode = keepAliveParams.get("mode").asQByteArray();
		if(mode.isEmpty() || mode == "idle")
		{
			keepAliveMode = Idle;
		}
		else if(mode == "interval")
		{
			keepAliveMode = Interval;
		}
		else
		{
			setError(ok, errorMessage, QString("no such Grip-Keep-Alive mode '%1'").arg(QString::fromUtf8(mode)));
			return Instruct();
		}

		if(keepAliveParams.contains("timeout"))
		{
			bool x;
			keepAliveTimeout = keepAliveParams.get("timeout").asQByteArray().toInt(&x);
			if(!x)
			{
				setError(ok, errorMessage, "failed to parse Grip-Keep-Alive timeout value");
				return Instruct();
			}

			if(keepAliveTimeout < 0)
			{
				setError(ok, errorMessage, "Grip-Keep-Alive timeout has invalid value");
				return Instruct();
			}
		}
		else
		{
			keepAliveTimeout = DEFAULT_RESPONSE_TIMEOUT;
		}

		QByteArray format = keepAliveParams.get("format").asQByteArray();
		if(format.isEmpty() || format == "raw")
		{
			keepAliveData = val;
		}
		else if(format == "cstring")
		{
			keepAliveData = unescape(val);
			if(keepAliveData.isNull())
			{
				setError(ok, errorMessage, "failed to parse Grip-Keep-Alive cstring format");
				return Instruct();
			}
		}
		else if(format == "base64")
		{
			keepAliveData = QByteArray::fromBase64(val);
		}
		else
		{
			setError(ok, errorMessage, QString("no such Grip-Keep-Alive format '%1'").arg(QString::fromUtf8(format)));
			return Instruct();
		}
	}

	QList<HttpHeaderParameters> metaParams = response.headers.getAllAsParameters("Grip-Set-Meta", HttpHeaders::ParseAllParameters);
	foreach(const HttpHeaderParameters &metaParam, metaParams)
	{
		if(metaParam.isEmpty())
		{
			setError(ok, errorMessage, "Grip-Set-Meta cannot be empty");
			return Instruct();
		}

		QString key = QString::fromUtf8(metaParam[0].first.asQByteArray());
		QString val = QString::fromUtf8(metaParam[0].second.asQByteArray());

		meta[key] = val;
	}

	newResponse = response;

	QByteArray statusHeader = response.headers.get("Grip-Status").asQByteArray();
	if(!statusHeader.isEmpty())
	{
		QByteArray codeStr;
		QByteArray reason;

		int at = statusHeader.indexOf(' ');
		if(at != -1)
		{
			codeStr = statusHeader.mid(0, at);
			reason = statusHeader.mid(at + 1);
		}
		else
		{
			codeStr = statusHeader;
		}

		bool _ok;
		newResponse.code = codeStr.toInt(&_ok);
		if(!_ok || newResponse.code < 0 || newResponse.code > 999)
		{
			setError(ok, errorMessage, "Grip-Status contains invalid status code");
			return Instruct();
		}

		newResponse.reason = reason;
	}

	QUrl nextLink;
	int nextLinkTimeout = -1;
	QUrl goneLink;
	foreach(const HttpHeaderParameters &params, response.headers.getAllAsParameters("Grip-Link"))
	{
		if(params.count() < 2)
			continue;

		QByteArray linkParam = params[0].first.asQByteArray();
		if(linkParam.length() <= 2 || linkParam[0] != '<' || linkParam[linkParam.length() - 1] != '>')
		{
			setError(ok, errorMessage, "failed to parse Grip-Link value");
			return Instruct();
		}

		QUrl link = QUrl::fromEncoded(linkParam.mid(1, linkParam.length() - 2));
		if(!link.isValid())
		{
			setError(ok, errorMessage, "Grip-Link contains invalid link");
			return Instruct();
		}

		QByteArray rel = params.get("rel").asQByteArray();
		if(rel == "next")
		{
			nextLink = link;

			if(params.contains("timeout"))
			{
				bool x;
				nextLinkTimeout = params.get("timeout").asQByteArray().toInt(&x);
				if(!x)
				{
					setError(ok, errorMessage, "failed to parse Grip-Link timeout value");
					return Instruct();
				}

				if(nextLinkTimeout < 0)
				{
					setError(ok, errorMessage, "Grip-Link timeout has invalid value");
					return Instruct();
				}
			}
			else
			{
				nextLinkTimeout = DEFAULT_NEXTLINK_TIMEOUT;
			}
		}
		else if(rel == "gone")
		{
			goneLink = link;
		}
	}

	newResponse.headers.clear();
	foreach(const HttpHeader &h, response.headers)
	{
		// Strip out grip headers
		if(qstrnicmp(h.first.data(), "Grip-", 5) == 0)
			continue;

		if(!exposeHeaders.isEmpty())
		{
			bool found = false;
			foreach(const QByteArray &e, exposeHeaders)
			{
				if(qstricmp(e.data(), h.first.data()) == 0)
				{
					found = true;
					break;
				}
			}

			if(!found)
				continue;
		}

		newResponse.headers += HttpHeader(h.first, h.second);
	}

	QByteArray contentType = response.headers.getAsFirstParameter("Content-Type").asQByteArray();
	if(contentType == "application/grip-instruct")
	{
		if(response.code != 200)
		{
			setError(ok, errorMessage, "response code for application/grip-instruct content must be 200");
			return Instruct();
		}

		QJsonParseError e;
		QJsonDocument doc = QJsonDocument::fromJson(response.body, &e);
		if(e.error != QJsonParseError::NoError)
		{
			setError(ok, errorMessage, "failed to parse application/grip-instruct content as JSON");
			return Instruct();
		}

		if(!doc.isObject())
		{
			setError(ok, errorMessage, "instruct must be an object");
			return Instruct();
		}

		QVariantMap minstruct = doc.object().toVariantMap();

		bool ok_;

		if(minstruct.contains("hold"))
		{
			if(typeId(minstruct["hold"]) != QMetaType::QVariantMap)
			{
				setError(ok, errorMessage, "instruct contains 'hold' with wrong type");
				return Instruct();
			}

			QString pn = "hold";

			QVariant vhold = minstruct["hold"];

			QString modeStr = getString(vhold, pn, "mode", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			if(!modeStr.isNull())
			{
				if(modeStr == "response")
				{
					holdMode = ResponseHold;
				}
				else if(modeStr == "stream")
				{
					holdMode = StreamHold;
				}
				else
				{
					setError(ok, errorMessage, "hold 'mode' must be set to either 'response' or 'stream'");
					return Instruct();
				}
			}
			else
			{
				holdMode = ResponseHold;
			}

			QVariantList vchannels = getList(vhold, pn, "channels", true, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			foreach(const QVariant &vchannel, vchannels)
			{
				QString cpn = "channel";
				Channel c;

				c.name = getString(vchannel, cpn, "name", true, &ok_, errorMessage);
				if(!ok_)
				{
					if(ok)
						*ok = false;
					return Instruct();
				}

				c.prevId = getString(vchannel, cpn, "prev-id", false, &ok_, errorMessage);
				if(!ok_)
				{
					if(ok)
						*ok = false;
					return Instruct();
				}

				QVariantList vfilters = getList(vchannel, cpn, "filters", false, &ok_, errorMessage);
				if(!ok_)
				{
					if(ok)
						*ok = false;
					return Instruct();
				}

				foreach(const QVariant &vfilter, vfilters)
				{
					QString filter = getString(vfilter, &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, "filters contains value with wrong type");
						return Instruct();
					}

					c.filters += filter;
				}

				channels += c;
			}

			if(keyedObjectContains(vhold, "timeout"))
			{
				QVariant vtimeout = keyedObjectGetValue(vhold, "timeout");
				if(!canConvert(vtimeout, QMetaType::Int))
				{
					setError(ok, errorMessage, QString("%1 contains 'timeout' with wrong type").arg(pn));
					return Instruct();
				}

				timeout = vtimeout.toInt();

				if(timeout < 0)
				{
					setError(ok, errorMessage, QString("%1 contains 'timeout' with invalid value").arg(pn));
					return Instruct();
				}
			}

			QVariant vka = getKeyedObject(vhold, pn, "keep-alive", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			if(isKeyedObject(vka))
			{
				QString kpn = "keep-alive";

				if(keyedObjectContains(vka, "content-bin"))
				{
					QString contentBin = getString(vka, kpn, "content-bin", false, &ok_, errorMessage);
					if(!ok_)
					{
						if(ok)
							*ok = false;
						return Instruct();
					}

					keepAliveData = QByteArray::fromBase64(contentBin.toUtf8());
				}
				else if(keyedObjectContains(vka, "content"))
				{
					QVariant vcontent = keyedObjectGetValue(vka, "content");
					if(typeId(vcontent) == QMetaType::QByteArray)
						keepAliveData = vcontent.toByteArray();
					else if(typeId(vcontent) == QMetaType::QString)
						keepAliveData = vcontent.toString().toUtf8();
					else
					{
						setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(kpn));
						return Instruct();
					}
				}

				if(keyedObjectContains(vka, "timeout"))
				{
					QVariant vtimeout = keyedObjectGetValue(vka, "timeout");
					if(!canConvert(vtimeout, QMetaType::Int))
					{
						setError(ok, errorMessage, QString("%1 contains 'timeout' with wrong type").arg(kpn));
						return Instruct();
					}

					keepAliveTimeout = vtimeout.toInt();

					if(keepAliveTimeout < 0)
					{
						setError(ok, errorMessage, QString("%1 contains 'timeout' with invalid value").arg(kpn));
						return Instruct();
					}
				}
				else
				{
					keepAliveTimeout = 55;
				}
			}

			QVariant vmeta = getKeyedObject(vhold, pn, "meta", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			if(vmeta.isValid())
			{
				if(typeId(vmeta) == QMetaType::QVariantHash)
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
							return Instruct();
						}

						meta[key] = val;
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
							return Instruct();
						}

						meta[key] = val;
					}
				}
			}
		}

		newResponse.headers.clear();
		newResponse.body.clear();

		if(minstruct.contains("response"))
		{
			if(typeId(minstruct["response"]) != QMetaType::QVariantMap)
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			QVariant in = minstruct["response"];

			QString pn = "response";

			if(keyedObjectContains(in, "code"))
			{
				QVariant vcode = keyedObjectGetValue(in, "code");
				if(!canConvert(vcode, QMetaType::Int))
				{
					setError(ok, errorMessage, QString("%1 contains 'code' with wrong type").arg(pn));
					return Instruct();
				}

				newResponse.code = vcode.toInt();

				if(newResponse.code < 0 || newResponse.code > 999)
				{
					setError(ok, errorMessage, QString("%1 contains 'code' with invalid value").arg(pn));
					return Instruct();
				}

				// If code was supplied in json instruct, then
				// we need to clear the default reason
				newResponse.reason.clear();
			}

			QString reasonStr = getString(in, pn, "reason", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			if(!reasonStr.isEmpty())
				newResponse.reason = reasonStr.toUtf8();

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
							return Instruct();
						}

						QVariantList lheader = vheader.toList();
						if(lheader.count() != 2)
						{
							setError(ok, errorMessage, "headers contains list with wrong number of elements");
							return Instruct();
						}

						QString name = getString(lheader[0], &ok_);
						if(!ok_)
						{
							setError(ok, errorMessage, "header contains name element with wrong type");
							return Instruct();
						}

						QString val = getString(lheader[1], &ok_);
						if(!ok_)
						{
							setError(ok, errorMessage, "header contains value element with wrong type");
							return Instruct();
						}

						newResponse.headers += HttpHeader(name.toUtf8(), val.toUtf8());
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
								return Instruct();
							}

							newResponse.headers += HttpHeader(key.toUtf8(), val.toUtf8());
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
								return Instruct();
							}

							newResponse.headers += HttpHeader(key.toUtf8(), val.toUtf8());
						}
					}
				}
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'headers' with wrong type").arg(pn));
					return Instruct();
				}
			}

			if(keyedObjectContains(in, "body-bin"))
			{
				QString bodyBin = getString(in, pn, "body-bin", false, &ok_, errorMessage);
				if(!ok_)
				{
					if(ok)
						*ok = false;
					return Instruct();
				}

				newResponse.body = QByteArray::fromBase64(bodyBin.toUtf8());
			}
			else if(keyedObjectContains(in, "body"))
			{
				QVariant vcontent = keyedObjectGetValue(in, "body");
				if(typeId(vcontent) == QMetaType::QByteArray)
					newResponse.body = vcontent.toByteArray();
				else if(typeId(vcontent) == QMetaType::QString)
					newResponse.body = vcontent.toString().toUtf8();
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'body' with wrong type").arg(pn));
					return Instruct();
				}
			}
		}
	}

	if(newResponse.reason.isEmpty())
		newResponse.reason = StatusReasons::getReason(newResponse.code);

	if(timeout == -1)
		timeout = DEFAULT_RESPONSE_TIMEOUT;

	timeout = qMax(timeout, MINIMUM_RESPONSE_TIMEOUT);

	if(keepAliveTimeout != -1)
	{
		if(keepAliveTimeout < 1)
			keepAliveTimeout = 1;
	}

	Instruct i;
	i.holdMode = holdMode;
	i.channels = channels;
	i.timeout = timeout;
	i.exposeHeaders = exposeHeaders;
	i.keepAliveMode = keepAliveMode;
	i.keepAliveData = keepAliveData;
	i.keepAliveTimeout = keepAliveTimeout;
	i.meta = meta;
	i.response = newResponse;
	i.nextLink = nextLink;
	i.nextLinkTimeout = nextLinkTimeout;
	i.goneLink = goneLink;

	if(ok)
		*ok = true;
	return i;
}

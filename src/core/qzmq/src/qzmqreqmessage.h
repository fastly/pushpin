/*
 * Copyright (C) 2012 Justin Karneges
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef QZMQREQMESSAGE_H
#define QZMQREQMESSAGE_H

namespace QZmq {

class ReqMessage
{
public:
	ReqMessage()
	{
	}

	ReqMessage(const QList<QByteArray> &headers, const QList<QByteArray> &content) :
		headers_(headers),
		content_(content)
	{
	}

	ReqMessage(const QList<QByteArray> &rawMessage)
	{
		bool collectHeaders = true;
		foreach(const QByteArray &part, rawMessage)
		{
			if(part.isEmpty())
			{
				collectHeaders = false;
				continue;
			}

			if(collectHeaders)
				headers_ += part;
			else
				content_ += part;
		}
	}

	bool isNull() const { return headers_.isEmpty() && content_.isEmpty(); }

	QList<QByteArray> headers() const { return headers_; }
	QList<QByteArray> content() const { return content_; }

	ReqMessage createReply(const QList<QByteArray> &content)
	{
		return ReqMessage(headers_, content);
	}

	QList<QByteArray> toRawMessage() const
	{
		QList<QByteArray> out;
		out += headers_;
		out += QByteArray();
		out += content_;
		return out;
	}

private:
	QList<QByteArray> headers_;
	QList<QByteArray> content_;
};

}

#endif

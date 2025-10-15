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

#ifndef ZMQREQMESSAGE_H
#define ZMQREQMESSAGE_H

class CowByteArrayList;

class ZmqReqMessage
{
public:
	ZmqReqMessage()
	{
	}

	ZmqReqMessage(const CowByteArrayList &headers, const CowByteArrayList &content) :
		headers_(headers),
		content_(content)
	{
	}

	ZmqReqMessage(const CowByteArrayList &rawMessage)
	{
		bool collectHeaders = true;
		for(CowByteArrayConstRef part : std::as_const(rawMessage))
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

	CowByteArrayList headers() const { return headers_; }
	CowByteArrayList content() const { return content_; }

	ZmqReqMessage createReply(const CowByteArrayList &content)
	{
		return ZmqReqMessage(headers_, content);
	}

	CowByteArrayList toRawMessage() const
	{
		CowByteArrayList out;
		out += headers_;
		out += CowByteArray();
		out += content_;
		return out;
	}

private:
	CowByteArrayList headers_;
	CowByteArrayList content_;
};

#endif

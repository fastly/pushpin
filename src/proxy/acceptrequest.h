/*
 * Copyright (C) 2015 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#ifndef ACCEPTREQUEST_H
#define ACCEPTREQUEST_H

#include "packet/httpresponsedata.h"
#include "zrpcrequest.h"

class AcceptData;
class ZrpcManager;

class AcceptRequest : public ZrpcRequest
{
public:
	class ResponseData
	{
	public:
		bool accepted;
		HttpResponseData response;

		ResponseData() :
			accepted(false)
		{
		}
	};

	AcceptRequest(ZrpcManager *manager);
	~AcceptRequest();

	ResponseData result() const;

	void start(const AcceptData &adata);

protected:
	virtual void onSuccess();

private:
	class Private;
	std::unique_ptr<Private> d;
};

#endif

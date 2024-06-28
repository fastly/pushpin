/*
 * Copyright (C) 2014 Fanout, Inc.
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

#ifndef LAYERTRACKER_H
#define LAYERTRACKER_H

#include <QList>

class LayerTracker
{
public:
	LayerTracker();

	void reset();

	void addPlain(int plain);
	void specifyEncoded(int encoded, int plain);
	int finished(int encoded);

private:
	class Item
	{
	public:
		int plain;
		int encoded;
	};

	int plain_;
	QList<Item> items_;
};

#endif

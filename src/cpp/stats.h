/*
 * Copyright (C) 2022-2023 Fanout, Inc.
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

#ifndef STATS_H
#define STATS_H

#include <QtGlobal>
#include <assert.h>
#include <string.h>

#define STATS_COUNTERS_MAX 12

namespace Stats {

// keep in sync with STATS_COUNTERS_MAX above
enum Counter {
    ClientHeaderBytesReceived  =  0,
    ClientHeaderBytesSent      =  1,
    ClientContentBytesReceived =  2,
    ClientContentBytesSent     =  3,
    ClientMessagesReceived     =  4,
    ClientMessagesSent         =  5,
    ServerHeaderBytesReceived  =  6,
    ServerHeaderBytesSent      =  7,
    ServerContentBytesReceived =  8,
    ServerContentBytesSent     =  9,
    ServerMessagesReceived     = 10,
    ServerMessagesSent         = 11,
	WsErrors                   = 12,
};

class Counters
{
public:
    Counters()
    {
        reset();
    }

    bool isEmpty() const
    {
        return _empty;
    }

    void reset()
    {
        for(int n = 0; n < STATS_COUNTERS_MAX; ++n)
            _values[n] = 0;

        _empty = true;
    }

    quint32 get(Counter c)
    {
        int index = (int)c;

        assert(index >= 0 && index < STATS_COUNTERS_MAX);

        return _values[index];
    }

    void inc(Counter c, quint32 count = 1)
    {
        int index = (int)c;

        assert(index >= 0 && index < STATS_COUNTERS_MAX);

        _values[index] += count;

        if(count > 0)
            _empty = false;
    }

    void add(const Counters &other);

private:
    quint32 _values[STATS_COUNTERS_MAX];
    bool _empty;
};

}

#endif

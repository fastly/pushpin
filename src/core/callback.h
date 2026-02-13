/*
 * Copyright (C) 2023 Fanout, Inc.
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

#ifndef CALLBACK_H
#define CALLBACK_H

#include <assert.h>
#include <QList>

template <typename T> class Callback
{
public:
    typedef void (*CallbackFunc)(void *data, T);

    Callback() :
        activeCalls_(0),
        destroyed_(0)
    {
    }

    ~Callback()
    {
        if(destroyed_)
            *destroyed_ = true;
    }

    void add(CallbackFunc cb, void *data)
    {
        targets_ += Target(cb, data);
    }

    void remove(void *data)
    {
        // Mark for removal, but don't actually remove
        for(int n = 0; n < targets_.count(); ++n)
        {
            Target &t = targets_[n];

            if(t.second == data)
            {
                t.second = 0;
            }
        }

        // Only actually remove if not in the middle of a call
        if(activeCalls_ == 0)
        {
            removeMarked();
        }
    }

    void call(T value)
    {
        activeCalls_ += 1;

        for(int n = 0; n < targets_.count(); ++n)
        {
            const Target &t = targets_[n];

            // Skip if marked for removal
            if(!t.second)
            {
                continue;
            }

            CallbackFunc f = t.first;
            void *data = t.second;

            bool *prevDestroyed = destroyed_;

            bool destroyed = false;
            destroyed_ = &destroyed;

            f(data, value);

            if(destroyed)
            {
                if(prevDestroyed)
                {
                    *prevDestroyed = true;
                }

                return;
            }

            destroyed_ = prevDestroyed;
        }

        assert(activeCalls_ >= 1);
        activeCalls_ -= 1;

        if(activeCalls_ == 0)
        {
            removeMarked();
        }
    }

private:
    typedef QPair<CallbackFunc, void *> Target;
    QList<Target> targets_;
    bool activeCalls_;
    bool *destroyed_;

    void removeMarked()
    {
        assert(activeCalls_ == 0);

        for(int n = 0; n < targets_.count(); ++n)
        {
            if(!targets_[n].second)
            {
                targets_.removeAt(n);
                --n; // Adjust position
            }
        }
    }
};

#endif

/*
 * Copyright (C) 2019 Fanout, Inc.
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

#ifndef WSCONTROL_H
#define WSCONTROL_H

#include "websocket.h"

namespace WsControl {

enum KeepAliveMode { NoKeepAlive, Idle, Interval };

class AutoRespondConfig {
public:
    WebSocket::Frame::Type matchType;
    QByteArray matchContent;
    QByteArray matchContentPtr;
    WebSocket::Frame::Type type;
    QByteArray content;

    AutoRespondConfig() : matchType((WebSocket::Frame::Type)-1), type((WebSocket::Frame::Type)-1) {}

    /// Returns true if the config has no matching criteria.
    bool isEmpty() const { return (((int)matchType) < 0 && matchContent.isNull()); }

    /// Returns true if the config has matching critera and response data.
    bool isEnabled() const { return (!isEmpty() && (((int)type) >= 0 || !content.isNull())); }

    /// Returns true if this config has the same matching criteria as `other`.
    bool matches(const AutoRespondConfig &other) {
        return (matchType == other.matchType &&
                ((matchContent.isNull() && other.matchContent.isNull()) ||
                 (!matchContent.isNull() && !other.matchContent.isNull() &&
                  matchContent == other.matchContent)) &&
                matchContentPtr == other.matchContentPtr);
    }
};

} // namespace WsControl

#endif

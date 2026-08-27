/*
 * Copyright (C) 2026 Fastly, Inc.
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
 */

#ifndef DATETIME_H
#define DATETIME_H

#include <chrono>
#include <cstdint>

// Date/time type wrapping std::chrono::system_clock::time_point.
class DateTime {
public:
    using time_point = std::chrono::system_clock::time_point;

    DateTime() = default;

    static DateTime currentDateTimeUtc() {
        DateTime dt;
        dt.inner_ = std::chrono::system_clock::now();
        return dt;
    }

    static int64_t currentMSecsSinceEpoch() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    int64_t toSecsSinceEpoch() const {
        return std::chrono::duration_cast<std::chrono::seconds>(inner_.time_since_epoch()).count();
    }

    DateTime addMSecs(int64_t msecs) const {
        DateTime dt;
        dt.inner_ = inner_ + std::chrono::milliseconds(msecs);
        return dt;
    }

    int64_t msecsTo(const DateTime &other) const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(other.inner_ - inner_).count();
    }

    friend bool operator==(const DateTime &a, const DateTime &b) { return a.inner_ == b.inner_; }
    friend bool operator!=(const DateTime &a, const DateTime &b) { return a.inner_ != b.inner_; }
    friend bool operator<(const DateTime &a, const DateTime &b) { return a.inner_ < b.inner_; }
    friend bool operator<=(const DateTime &a, const DateTime &b) { return a.inner_ <= b.inner_; }
    friend bool operator>(const DateTime &a, const DateTime &b) { return a.inner_ > b.inner_; }
    friend bool operator>=(const DateTime &a, const DateTime &b) { return a.inner_ >= b.inner_; }

private:
    time_point inner_;
};

#endif

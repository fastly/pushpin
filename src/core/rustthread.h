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

#ifndef RUST_THREAD_H
#define RUST_THREAD_H

#include "rust/bindings.h"
#include <functional>
#include <memory>
#include <string>

namespace RustThread {

class JoinHandle {
public:
    JoinHandle();

    void join();

private:
    std::unique_ptr<ffi::ThreadJoinHandle, void (*)(ffi::ThreadJoinHandle *)> handle_;

    JoinHandle(ffi::ThreadJoinHandle *handle);

    friend JoinHandle spawn(std::function<void()> f, const std::string &name);
};

JoinHandle spawn(std::function<void()> f, const std::string &name = "");

} // namespace RustThread

#endif

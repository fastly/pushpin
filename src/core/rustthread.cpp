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

#include "rustthread.h"

#include <assert.h>

static void call_and_delete_function(void *ctx) {
    std::function<void()> *f = reinterpret_cast<std::function<void()> *>(ctx);
    (*f)();
    delete f;
}

namespace RustThread {

JoinHandle::JoinHandle() : handle_(nullptr, ffi::thread_forget) {}

JoinHandle::JoinHandle(ffi::ThreadJoinHandle *handle) : handle_(handle, ffi::thread_forget) {}

void JoinHandle::join() {
    if (handle_) {
        ffi::thread_join(handle_.release());
    }
}

JoinHandle spawn(std::function<void()> f, const std::string &name) {
    // Move the function to the heap. The spawned thread will take care of destruction.
    std::function<void()> *f_heap = new std::function<void()>(f);

    const char *name_c = nullptr;
    if (!name.empty())
        name_c = name.c_str();

    ffi::ThreadJoinHandle *handle =
        ffi::thread_spawn(call_and_delete_function, reinterpret_cast<void *>(f_heap), name_c);
    assert(handle);

    return JoinHandle(handle);
}

} // namespace RustThread

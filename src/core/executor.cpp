/*
 * Copyright (C) 2025 Fastly, Inc.
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

#include "executor.h"

Executor::Executor(int tasksMax) :
    inner_(ffi::executor_create(tasksMax))
{
}

Executor::~Executor()
{
    ffi::executor_destroy(inner_);
}

int Executor::park_cb(void *ctx, int ms)
{
    std::function<bool (std::optional<int>)> *park = (std::function<bool (std::optional<int>)> *)ctx;

    std::optional<int> x;
    if(ms >= 0)
        x = ms;

    if(!(*park)(x))
        return -1;

    return 0;
}

bool Executor::run(std::function<bool (std::optional<int>)> park)
{
    if(ffi::executor_run(inner_, park_cb, &park) != 0)
        return false;

    return true;
}

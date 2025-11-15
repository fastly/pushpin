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

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <optional>
#include <functional>
#include "rust/bindings.h"

class Executor
{
public:
	Executor(int tasksMax);
	~Executor();

	bool run(std::function<bool (std::optional<int>)> park);

	/// Spawns `fut` on the executor in the current thread. Returns true on
	/// success or false on error. An error can occur if there is no executor in
	/// the current thread or if the executor is at capacity. This function takes
	/// ownership of `fut` regardless of whether spawning is successful.
	static bool currentSpawn(ffi::UnitFuture *fut);

private:
	ffi::Executor *inner_;

	static int park_cb(void *ctx, int ms);
};

#endif

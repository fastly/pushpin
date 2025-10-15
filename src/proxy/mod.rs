/*
 * Copyright (C) 2024-2025 Fastly, Inc.
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

pub mod cliargs;

#[cfg(test)]
mod tests {
    use crate::core::test::{run_cpp, TestException};
    use crate::ffi;

    fn websocketoverhttp_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::websocketoverhttp_test(out_ex) == 0 }
    }

    fn routesfile_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::routesfile_test(out_ex) == 0 }
    }

    fn proxyengine_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::proxyengine_test(out_ex) == 0 }
    }

    fn proxyargs_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::proxyargs_test(out_ex) == 0 }
    }

    #[test]
    fn websocketoverhttp() {
        run_cpp(websocketoverhttp_test);
    }

    #[test]
    fn routesfile() {
        run_cpp(routesfile_test);
    }

    #[test]
    fn proxyengine() {
        run_cpp(proxyengine_test);
    }

    #[test]
    fn proxyargs() {
        run_cpp(proxyargs_test);
    }
}

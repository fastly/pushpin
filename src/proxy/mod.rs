/*
 * Copyright (C) 2024 Fastly, Inc.
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

#[cfg(test)]
mod tests {
    use crate::core::{call_c_main, qtest};
    use crate::ffi;
    use std::ffi::OsStr;

    fn routesfile_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::routesfile_test, args) as u8 }
    }

    fn proxyengine_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::proxyengine_test, args) as u8 }
    }

    #[test]
    fn routesfile() {
        assert!(qtest::run(routesfile_test));
    }

    #[test]
    fn proxyengine() {
        assert!(qtest::run(proxyengine_test));
    }
}

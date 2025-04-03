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

#[cfg(test)]
mod tests {
    use crate::core::qtest;
    use crate::core::test::TestException;
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

    #[test]
    fn websocketoverhttp() {
        qtest::run_no_main(websocketoverhttp_test);
    }

    #[test]
    fn routesfile() {
        qtest::run_no_main(routesfile_test);
    }

    #[test]
    fn proxyengine() {
        qtest::run_no_main(proxyengine_test);
    }
}

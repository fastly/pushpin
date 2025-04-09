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
    use crate::core::test::{run_serial, TestException};
    use crate::ffi;

    fn filter_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::filter_test(out_ex) == 0 }
    }

    fn jsonpatch_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::jsonpatch_test(out_ex) == 0 }
    }

    fn instruct_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::instruct_test(out_ex) == 0 }
    }

    fn idformat_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::idformat_test(out_ex) == 0 }
    }

    fn publishformat_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::publishformat_test(out_ex) == 0 }
    }

    fn publishitem_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::publishitem_test(out_ex) == 0 }
    }

    fn handlerengine_test(out_ex: &mut TestException) -> bool {
        // SAFETY: safe to call
        unsafe { ffi::handlerengine_test(out_ex) == 0 }
    }

    #[test]
    fn filter() {
        run_serial(filter_test);
    }

    #[test]
    fn jsonpatch() {
        run_serial(jsonpatch_test);
    }

    #[test]
    fn instruct() {
        run_serial(instruct_test);
    }

    #[test]
    fn idformat() {
        run_serial(idformat_test);
    }

    #[test]
    fn publishformat() {
        run_serial(publishformat_test);
    }

    #[test]
    fn publishitem() {
        run_serial(publishitem_test);
    }

    #[test]
    fn handlerengine() {
        run_serial(handlerengine_test);
    }
}

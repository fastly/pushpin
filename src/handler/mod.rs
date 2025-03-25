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
        qtest::run_no_main(filter_test);
    }

    #[test]
    fn jsonpatch() {
        qtest::run_no_main(jsonpatch_test);
    }

    #[test]
    fn instruct() {
        qtest::run_no_main(instruct_test);
    }

    #[test]
    fn idformat() {
        qtest::run_no_main(idformat_test);
    }

    #[test]
    fn publishformat() {
        qtest::run_no_main(publishformat_test);
    }

    #[test]
    fn publishitem() {
        qtest::run_no_main(publishitem_test);
    }

    #[test]
    fn handlerengine() {
        qtest::run_no_main(handlerengine_test);
    }
}

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

    fn filter_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::filter_test, args) as u8 }
    }

    fn jsonpatch_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::jsonpatch_test, args) as u8 }
    }

    fn instruct_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::instruct_test, args) as u8 }
    }

    fn idformat_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::idformat_test, args) as u8 }
    }

    fn publishformat_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::publishformat_test, args) as u8 }
    }

    fn publishitem_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::publishitem_test, args) as u8 }
    }

    fn handlerengine_test(args: &[&OsStr]) -> u8 {
        // SAFETY: safe to call
        unsafe { call_c_main(ffi::handlerengine_test, args) as u8 }
    }

    #[test]
    fn filter() {
        assert!(qtest::run(filter_test));
    }

    #[test]
    fn jsonpatch() {
        assert!(qtest::run(jsonpatch_test));
    }

    #[test]
    fn instruct() {
        assert!(qtest::run(instruct_test));
    }

    #[test]
    fn idformat() {
        assert!(qtest::run(idformat_test));
    }

    #[test]
    fn publishformat() {
        assert!(qtest::run(publishformat_test));
    }

    #[test]
    fn publishitem() {
        assert!(qtest::run(publishitem_test));
    }

    #[test]
    fn handlerengine() {
        assert!(qtest::run(handlerengine_test));
    }
}

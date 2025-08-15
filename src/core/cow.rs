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

pub mod ffi {
    use std::sync::Arc;

    #[derive(Clone)]
    pub struct CowByteArray(Vec<u8>);

    #[no_mangle]
    pub extern "C" fn cow_byte_array_create() -> *const CowByteArray {
        let a = Arc::new(CowByteArray(vec![0]));

        Arc::into_raw(a)
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn cow_byte_array_copy(a: *const CowByteArray) -> *const CowByteArray {
        assert!(!a.is_null());

        Arc::increment_strong_count(a);

        a
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn cow_byte_array_destroy(a: *const CowByteArray) {
        if !a.is_null() {
            drop(Arc::from_raw(a));
        }
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn cow_byte_array_size(a: *const CowByteArray) -> libc::size_t {
        let a = a.as_ref().unwrap();

        a.0.len() - 1
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn cow_byte_array_data(a: *const CowByteArray) -> *const u8 {
        let a = a.as_ref().unwrap();

        a.0.as_ptr()
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn cow_byte_array_data_mut(a: *mut *const CowByteArray) -> *mut u8 {
        assert!(!a.is_null());

        let mut arc_a = Arc::from_raw(*a);
        let data = Arc::make_mut(&mut arc_a).0.as_mut_ptr();
        *a = Arc::into_raw(arc_a);

        data
    }

    #[allow(clippy::missing_safety_doc)]
    #[no_mangle]
    pub unsafe extern "C" fn cow_byte_array_resize(
        a: *mut *const CowByteArray,
        size: libc::size_t,
    ) {
        assert!(!a.is_null());

        let mut arc_a = Arc::from_raw(*a);

        {
            let size = size as usize;

            let a = Arc::make_mut(&mut arc_a);
            a.0.resize(size + 1, 0);
            a.0[size] = 0;
        }

        *a = Arc::into_raw(arc_a);
    }
}

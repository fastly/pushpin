/*
 * Copyright (C) 2021-2022 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
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
 *
 * $FANOUT_END_LICENSE$
 */

/// cbindgen:ignore
pub mod connmgr;
pub mod core;
/// cbindgen:ignore
pub mod handler;
/// cbindgen:ignore
pub mod proxy;
/// cbindgen:ignore
pub mod publish;
/// cbindgen:ignore
pub mod runner;

#[macro_export]
macro_rules! import_cpp {
    ($($tt:tt)*) => {
        #[link(name = "pushpin-cpp")]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "framework"),
            link(name = "QtNetwork", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "framework"),
            link(name = "Qt6Network", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "framework"),
            link(name = "Qt5Network", kind = "framework")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "dylib"),
            link(name = "QtNetwork", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "dylib"),
            link(name = "Qt6Network", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "dylib"),
            link(name = "Qt5Network", kind = "dylib")
        )]
        #[cfg_attr(target_os = "macos", link(name = "c++"))]
        #[cfg_attr(not(target_os = "macos"), link(name = "stdc++"))]
        extern "C" {
            $($tt)*
        }
    };
}

#[macro_export]
macro_rules! import_cpptest {
    ($($tt:tt)*) => {
        #[link(name = "pushpin-cpptest")]
        #[link(name = "pushpin-cpp")]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "framework"),
            link(name = "QtNetwork", kind = "framework"),
            link(name = "QtTest", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "framework"),
            link(name = "Qt6Network", kind = "framework"),
            link(name = "Qt6Test", kind = "framework")
        )]
        #[cfg_attr(
            all(target_os = "macos", qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "framework"),
            link(name = "Qt5Network", kind = "framework"),
            link(name = "Qt5Test", kind = "framework")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt"),
            link(name = "QtCore", kind = "dylib"),
            link(name = "QtNetwork", kind = "dylib"),
            link(name = "QtTest", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt6"),
            link(name = "Qt6Core", kind = "dylib"),
            link(name = "Qt6Network", kind = "dylib"),
            link(name = "Qt6Test", kind = "dylib")
        )]
        #[cfg_attr(
            all(not(target_os = "macos"), qt_lib_prefix = "Qt5"),
            link(name = "Qt5Core", kind = "dylib"),
            link(name = "Qt5Network", kind = "dylib"),
            link(name = "Qt5Test", kind = "dylib")
        )]
        #[cfg_attr(target_os = "macos", link(name = "c++"))]
        #[cfg_attr(not(target_os = "macos"), link(name = "stdc++"))]
        extern "C" {
            $($tt)*
        }
    };
}

pub mod ffi {
    #[cfg(test)]
    import_cpptest! {
        pub fn httpheaders_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn jwt_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn routesfile_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn proxyengine_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn jsonpatch_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn instruct_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn idformat_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn publishformat_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn publishitem_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn handlerengine_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
        pub fn template_test(argc: libc::c_int, argv: *const *const libc::c_char) -> libc::c_int;
    }
}

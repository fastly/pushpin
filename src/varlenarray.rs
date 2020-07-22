/*
 * Copyright (C) 2020 Fanout, Inc.
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

#![allow(dead_code)]

use std::convert::TryFrom;
use std::fmt;
use std::str;

macro_rules! declare_vla {
    ($name:ident, $size:literal) => {
        pub struct $name<T> {
            data: [T; $size],
            len: usize,
        }

        impl<T> $name<T> {
            pub fn len(&self) -> usize {
                self.len
            }

            pub fn push(&mut self, value: T) {
                assert!(self.len < $size);

                self.data[self.len] = value;
                self.len += 1;
            }
        }

        impl<T> AsRef<[T]> for $name<T> {
            fn as_ref(&self) -> &[T] {
                &self.data[..self.len]
            }
        }

        impl<T: Copy + Default> TryFrom<&[T]> for $name<T> {
            type Error = ();

            fn try_from(src: &[T]) -> Result<Self, Self::Error> {
                if src.len() > $size {
                    return Err(());
                }

                let mut v = Self {
                    data: [T::default(); $size],
                    len: src.len(),
                };

                &v.data[..v.len].copy_from_slice(src);

                Ok(v)
            }
        }
    };
}

macro_rules! declare_vls {
    ($name:ident, $size:literal) => {
        pub struct $name {
            data: [u8; $size],
            len: usize,
        }

        impl $name {
            pub fn len(&self) -> usize {
                self.len
            }

            pub fn as_bytes(&self) -> &[u8] {
                &self.data[..self.len]
            }
        }

        impl AsRef<str> for $name {
            fn as_ref(&self) -> &str {
                unsafe { str::from_utf8_unchecked(&self.data[..self.len]) }
            }
        }

        impl TryFrom<&str> for $name {
            type Error = ();

            fn try_from(src: &str) -> Result<Self, Self::Error> {
                if src.len() > $size {
                    return Err(());
                }

                let mut v = Self {
                    data: [0; $size],
                    len: src.len(),
                };

                &v.data[..v.len].copy_from_slice(src.as_bytes());

                Ok(v)
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}", self.as_ref())
            }
        }
    };
}

declare_vla!(VarLenArray8, 8);
declare_vla!(VarLenArray64, 64);

declare_vls!(VarLenStr8, 8);
declare_vls!(VarLenStr16, 16);
declare_vls!(VarLenStr32, 32);

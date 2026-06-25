/*
 * Copyright (C) 2026 Fastly, Inc.
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

use std::ops::{Deref, DerefMut};
use std::sync::Arc;

/// Arc wrapper for Qt-like implicit sharing, with copy-on-write on mutation
pub struct CowArc<T>(Arc<T>);

impl<T> CowArc<T> {
    pub fn new(v: T) -> Self {
        Self(Arc::new(v))
    }
}

impl<T> Deref for CowArc<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl<T: Clone> DerefMut for CowArc<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        Arc::make_mut(&mut self.0)
    }
}

impl<T: Default> Default for CowArc<T> {
    fn default() -> Self {
        Self(Arc::new(T::default()))
    }
}

impl<T> Clone for CowArc<T> {
    #[inline(always)]
    fn clone(&self) -> Self {
        Self(Arc::clone(&self.0))
    }
}

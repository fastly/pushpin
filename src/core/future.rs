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

use std::future::Future;
use std::mem;
use std::pin::Pin;

pub trait SizedFuture: Future {
    fn size(&self) -> usize;

    fn into_future<'a>(self: Pin<Box<Self>>) -> Pin<Box<dyn Future<Output = Self::Output> + 'a>>
    where
        Self: 'a;
}

impl<T: Future> SizedFuture for T {
    fn size(&self) -> usize {
        mem::size_of::<Self>()
    }

    fn into_future<'a>(self: Pin<Box<Self>>) -> Pin<Box<dyn Future<Output = Self::Output> + 'a>>
    where
        Self: 'a,
    {
        self
    }
}

pub mod ffi {
    use super::*;

    pub struct UnitFuture(pub Pin<Box<dyn SizedFuture<Output = ()>>>);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::future::pending;

    #[test]
    fn sized_future() {
        // A future that should be at least 100 bytes
        let fut = Box::pin(async {
            let mut arr = [0u8; 100];
            pending::<()>().await;
            arr[arr.len() - 1] = 1;
            println!("{}", arr[arr.len() - 1]);
        });

        let size = mem::size_of_val(&*fut);
        assert!(size >= 100);

        let fut = fut as Pin<Box<dyn SizedFuture<Output = ()>>>;
        assert_eq!((*fut).size(), size);

        let _fut: Pin<Box<dyn Future<Output = ()>>> = fut.into_future();
    }
}

/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

use crate::core::shuffle::shuffle;
use paste::paste;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll};

fn range_unordered(dest: &mut [usize]) -> &[usize] {
    for (index, v) in dest.iter_mut().enumerate() {
        *v = index;
    }

    shuffle(dest);

    dest
}

fn map_poll<F, W, V>(cx: &mut Context, fut: &mut F, wrap_func: W) -> Poll<V>
where
    F: Future + Unpin,
    W: FnOnce(F::Output) -> V,
{
    match Pin::new(fut).poll(cx) {
        Poll::Ready(v) => Poll::Ready(wrap_func(v)),
        Poll::Pending => Poll::Pending,
    }
}

macro_rules! declare_select {
    ($count: literal, ( $($num:literal),* )) => {
        paste! {
            pub enum [<Select $count>]<$([<O $num>], )*> {
                $(
                    [<R $num>]([<O $num>]),
                )*
            }

            pub struct [<Select $count Future>]<$([<F $num>], )*> {
                $(
                    [<f $num>]: [<F $num>],
                )*
            }

            impl<$([<F $num>], )*> Future for [<Select $count Future>]<$([<F $num>], )*>
            where
                $(
                    [<F $num>]: Future + Unpin,
                )*
            {
                type Output = [<Select $count>]<$([<F $num>]::Output, )*>;

                fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
                    let mut indexes = [0; $count];

                    for i in range_unordered(&mut indexes) {
                        let s = &mut *self;

                        let p = match i + 1 {
                            $(
                                $num => map_poll(cx, &mut s.[<f $num>], |v| [<Select $count>]::[<R $num>](v)),
                            )*
                            _ => unreachable!(),
                        };

                        if p.is_ready() {
                            return p;
                        }
                    }

                    Poll::Pending
                }
            }

            #[allow(clippy::too_many_arguments)]
            pub fn [<select_ $count>]<$([<F $num>], )*>(
                $(
                    [<f $num>]: [<F $num>],
                )*
            ) -> [<Select $count Future>]<$([<F $num>], )*>
            where
                $(
                    [<F $num>]: Future + Unpin,
                )*
            {
                [<Select $count Future>] {
                    $(
                        [<f $num>],
                    )*
                }
            }
        }
    }
}

declare_select!(2, (1, 2));
declare_select!(3, (1, 2, 3));
declare_select!(4, (1, 2, 3, 4));
declare_select!(5, (1, 2, 3, 4, 5));
declare_select!(6, (1, 2, 3, 4, 5, 6));
declare_select!(8, (1, 2, 3, 4, 5, 6, 7, 8));
declare_select!(9, (1, 2, 3, 4, 5, 6, 7, 8, 9));
declare_select!(10, (1, 2, 3, 4, 5, 6, 7, 8, 9, 10));

pub struct SelectSliceFuture<'a, F> {
    futures: &'a mut [F],
    scratch: &'a mut Vec<usize>,
}

impl<F, O> Future for SelectSliceFuture<'_, F>
where
    F: Future<Output = O> + Unpin,
{
    type Output = (usize, F::Output);

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = &mut *self;

        let indexes = &mut s.scratch;
        indexes.resize(s.futures.len(), 0);

        for i in range_unordered(&mut indexes[..s.futures.len()]) {
            if let Poll::Ready(v) = Pin::new(&mut s.futures[*i]).poll(cx) {
                return Poll::Ready((*i, v));
            }
        }

        Poll::Pending
    }
}

pub fn select_slice<'a, F, O>(
    futures: &'a mut [F],
    scratch: &'a mut Vec<usize>,
) -> SelectSliceFuture<'a, F>
where
    F: Future<Output = O> + Unpin,
{
    if futures.len() > scratch.capacity() {
        panic!(
            "select_slice scratch is not large enough: {}, need {}",
            scratch.capacity(),
            futures.len()
        );
    }

    SelectSliceFuture { futures, scratch }
}

pub struct SelectOptionFuture<F> {
    fut: Option<F>,
}

impl<F, O> Future for SelectOptionFuture<F>
where
    F: Future<Output = O> + Unpin,
{
    type Output = O;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        let s = &mut *self;

        match Pin::new(&mut s.fut).as_pin_mut() {
            Some(f) => f.poll(cx),
            None => Poll::Pending,
        }
    }
}

pub fn select_option<F, O>(fut: Option<F>) -> SelectOptionFuture<F>
where
    F: Future<Output = O>,
{
    SelectOptionFuture { fut }
}

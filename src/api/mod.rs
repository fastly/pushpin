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

use log::{debug, info};
use signal_hook;
use signal_hook::consts::TERM_SIGNALS;
use signal_hook::iterator::Signals;
use std::error::Error;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;

pub struct Config {
    pub _workers: usize,
}

pub struct App;

impl App {
    pub fn new(_config: &Config) -> Result<Self, String> {
        Ok(Self)
    }

    pub fn wait_for_term(&self) {
        let mut signals = Signals::new(TERM_SIGNALS).unwrap();

        let term_now = Arc::new(AtomicBool::new(false));

        // Ensure two term signals in a row causes the app to immediately exit
        for signal_type in TERM_SIGNALS {
            signal_hook::flag::register_conditional_shutdown(
                *signal_type,
                1, // Exit code
                Arc::clone(&term_now),
            )
            .unwrap();

            signal_hook::flag::register(*signal_type, Arc::clone(&term_now)).unwrap();
        }

        // Wait for termination
        let signal_type = signals.into_iter().next().unwrap();
        assert!(TERM_SIGNALS.contains(&signal_type));
    }
}

pub fn run(config: &Config) -> Result<(), Box<dyn Error>> {
    debug!("starting...");

    {
        let a = match App::new(config) {
            Ok(a) => a,
            Err(e) => {
                return Err(e.into());
            }
        };

        info!("started");

        a.wait_for_term();

        info!("stopping...");
    }

    debug!("stopped");

    Ok(())
}

use log::error;
use signal_hook::low_level::signal_name;
use std::io;
use std::mem;
use std::ptr;

fn set_signal_handler(
    signal: libc::c_int,
    handler: extern "C" fn(libc::c_int),
) -> Result<(), io::Error> {
    let sa = unsafe {
        let mut sa: libc::sigaction = mem::zeroed();

        libc::sigemptyset(&mut sa.sa_mask);
        sa.sa_sigaction = handler as usize;

        sa
    };

    if unsafe { libc::sigaction(signal, &sa, ptr::null_mut()) } != 0 {
        return Err(io::Error::last_os_error());
    }

    Ok(())
}

fn set_signal_handler_builtin(
    signal: libc::c_int,
    handler: libc::sighandler_t,
) -> Result<(), io::Error> {
    let sa = unsafe {
        let mut sa: libc::sigaction = mem::zeroed();

        libc::sigemptyset(&mut sa.sa_mask);
        sa.sa_sigaction = handler;

        sa
    };

    if unsafe { libc::sigaction(signal, &sa, ptr::null_mut()) } != 0 {
        return Err(io::Error::last_os_error());
    }

    Ok(())
}

extern "C" fn on_sigfatal(signal: libc::c_int) {
    match signal_name(signal) {
        Some(s) => eprintln!("received fatal signal {}", s),
        None => eprintln!("received fatal signal {}", signal),
    }

    let _ = set_signal_handler_builtin(signal, libc::SIG_DFL);

    eprintln!("backtrace:");
    eprintln!("{:?}", backtrace::Backtrace::new());

    unsafe { libc::raise(signal) };
}

fn try_set_signal_handler(signal: libc::c_int, handler: extern "C" fn(libc::c_int)) {
    let name = match signal_name(signal) {
        Some(s) => s.to_owned(),
        None => signal.to_string(),
    };

    if let Err(e) = set_signal_handler(signal, handler) {
        error!("failed to set signal {} handler: {}", name, e);
    }
}

pub fn setup_signal_handlers() {
    try_set_signal_handler(libc::SIGABRT, on_sigfatal);
    try_set_signal_handler(libc::SIGBUS, on_sigfatal);
    try_set_signal_handler(libc::SIGFPE, on_sigfatal);
    try_set_signal_handler(libc::SIGILL, on_sigfatal);
    try_set_signal_handler(libc::SIGSEGV, on_sigfatal);
}

mod ffi {
    use super::*;

    #[no_mangle]
    pub extern "C" fn backtrace_setup_signal_handlers() {
        setup_signal_handlers()
    }
}

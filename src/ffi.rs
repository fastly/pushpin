use crate::timer::TimerWheel;
use libc;

#[repr(C)]
pub struct ExpiredTimer {
    key: libc::c_int,
    user_data: libc::size_t,
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_create(capacity: libc::c_uint) -> *mut TimerWheel {
    let wheel = TimerWheel::new(capacity as usize);

    Box::into_raw(Box::new(wheel))
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_destroy(wheel: *mut TimerWheel) {
    if !wheel.is_null() {
        drop(Box::from_raw(wheel));
    }
}

#[no_mangle]
pub unsafe extern "C" fn timer_add(
    wheel: *mut TimerWheel,
    expires: u64,
    user_data: libc::size_t,
) -> libc::c_int {
    match TimerWheel::add(&mut *wheel, expires, user_data) {
        Ok(key) => key as libc::c_int,
        Err(_) => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn timer_remove(wheel: *mut TimerWheel, key: libc::c_int) {
    TimerWheel::remove(&mut *wheel, key as usize);
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_timeout(wheel: *mut TimerWheel) -> i64 {
    match TimerWheel::timeout(&*wheel) {
        Some(timeout) => timeout as i64,
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_update(wheel: *mut TimerWheel, curtime: u64) {
    TimerWheel::update(&mut *wheel, curtime);
}

#[no_mangle]
pub unsafe extern "C" fn timer_wheel_take_expired(wheel: *mut TimerWheel) -> ExpiredTimer {
    match TimerWheel::take_expired(&mut *wheel) {
        Some((key, user_data)) => ExpiredTimer {
            key: key as libc::c_int,
            user_data: user_data as libc::size_t,
        },
        None => ExpiredTimer {
            key: -1,
            user_data: 0,
        },
    }
}

//! This module provides Rust definitions of macros and structs used for BPF programs.
//! Additionally, some definitions are included that are seccomp-specific.
//!
//! ### Why not use the `seccomp` crate?
//!
//! The `seccomp` crate is built on `libseccomp`, is fairly unmaintained, and doesn't use bindgen
//! to bind underlying definitions either. Meanwhile, we only need a few definitions for ExecuteD's
//! seccomp program, so for now this is a sufficient stand-in.

// Some definitions, like BPF filter code constants, here might not get used today, but are
// included for completeness.
#![allow(dead_code)]

pub mod syscalls;

use log::{error, trace};
use nix::errno::Errno;
use std::convert::TryInto;
use std::sync::Mutex;

lazy_static::lazy_static! {
    static ref SECCOMP_ACTIVE: Mutex<bool> = Mutex::new(false);
}

macro_rules! SECCOMP_ALLOW {
    ( $($x:expr),* ) => {
        {
        let mut temp_vec = Vec::new();
        $(
        temp_vec.push(FilterInstruction::load_nr());
        temp_vec.push(FilterInstruction::jump(FilterCode::cmp_eq_const(), $x, 0, 1));
        temp_vec.push(FilterInstruction::allow());
        )*
        temp_vec
        }
    };
}

pub fn install_seccomp_connect_filter() {
    use self::syscalls::*;
    let mut syscall_allow_list = SECCOMP_ALLOW!(
        __NR_ACCEPT4,
        __NR_BIND,
        __NR_BRK,
        __NR_CLOCK_GETTIME,
        __NR_CLOCK_NANOSLEEP,
        __NR_CLONE,
        __NR_CLONE3,
        __NR_CLOSE,
        __NR_CONNECT,
        __NR_EPOLL_CREATE,
        __NR_EPOLL_CREATE1,
        __NR_EPOLL_CTL,
        __NR_EPOLL_WAIT,
        __NR_EVENTFD2,
        __NR_EXIT,
        __NR_EXIT_GROUP,
        __NR_FADVISE64,
        __NR_FCNTL,
        __NR_FSTAT,
        __NR_NEWFSTATAT,
        __NR_FUTEX,
        __NR_GET_ROBUST_LIST,
        __NR_GETCWD,
        __NR_GETDENTS,
        __NR_GETDENTS64,
        __NR_GETPEERNAME,
        __NR_GETPID,
        __NR_GETPPID,
        __NR_GETRANDOM,
        __NR_GETSOCKNAME,
        __NR_GETSOCKOPT,
        __NR_GETTID,
        __NR_GETTIMEOFDAY,
        __NR_GETUID,
        __NR_INOTIFY_INIT,
        __NR_INOTIFY_INIT1,
        __NR_INOTIFY_ADD_WATCH,
        __NR_INOTIFY_RM_WATCH,
        __NR_IOCTL,
        __NR_KILL,
        __NR_LISTEN,
        __NR_LSEEK,
        __NR_LSTAT,
        __NR_MADVISE,
        __NR_MMAP,
        __NR_MPROTECT,
        __NR_MREMAP,
        __NR_MUNMAP,
        __NR_NANOSLEEP,
        __NR_OPEN,
        __NR_OPENAT,
        __NR_PIPE2,
        __NR_POLL,
        __NR_PPOLL,
        __NR_PRCTL,
        __NR_PREAD64,
        __NR_READ,
        __NR_READLINK,
        __NR_RECVFROM,
        __NR_RECVMSG,
        __NR_RESTART_SYSCALL,
        __NR_RSEQ,
        __NR_RT_SIGACTION,
        __NR_RT_SIGPROCMASK,
        __NR_RT_SIGRETURN,
        __NR_SCHED_GETAFFINITY,
        __NR_SCHED_GETPARAM,
        __NR_SCHED_GET_PRIORITY_MAX,
        __NR_SCHED_GETSCHEDULER,
        __NR_SCHED_SETSCHEDULER,
        __NR_SCHED_YIELD,
        __NR_SENDFILE,
        __NR_SENDTO,
        __NR_SET_ROBUST_LIST,
        __NR_SETSOCKOPT,
        __NR_SHUTDOWN,
        __NR_SIGALTSTACK,
        __NR_SOCKETPAIR,
        __NR_STAT,
        __NR_STATFS,
        __NR_STATX,
        __NR_TGKILL,
        __NR_UNAME,
        __NR_UNLINK,
        __NR_WRITE,
        __NR_WRITEV
    );
    let socket_filter = vec![
        FilterInstruction::load_nr(),
        FilterInstruction::jump(FilterCode::cmp_eq_const(), __NR_SOCKET, 1, 0),
        /* socket is the last syscall we check, if this isn't it, kill the process */
        FilterInstruction::trap(),
        FilterInstruction::load_arg(0), // check that the domain is `UNIX`
        FilterInstruction::jump(
            FilterCode::cmp_eq_const(),
            nix::sys::socket::AddressFamily::Unix as u32,
            0, // if this is a unix socket request, fall through to allow
            1, // else branch to errno
        ),
        FilterInstruction::allow(),
        FilterInstruction::errno(libc::EPERM as u16),
    ];
    syscall_allow_list.extend(socket_filter);
    let load_res = load(&syscall_allow_list);
    if let Err(e) = load_res {
        error!("failed to install seccomp filter: {}", e);
        std::process::exit(1);
    }
    trace!("loaded socket(2) filter, non-Unix sockets are now denied");

    #[cfg(debug_assertions)]
    {
        match std::net::TcpStream::connect("127.0.0.1:80").err() {
            Some(err) => {
                if err.kind() != std::io::ErrorKind::PermissionDenied {
                    panic!("unexpected error exercising seccomp filter: {}", err);
                }
                // else we got a PermissionDenied, like we wanted
            }
            None => {
                panic!("erroneously able to create a TCP socket after installing seccomp filter to deny that permission");
            }
        }
    }
}

fn activate_seccomp() -> Result<(), Errno> {
    let activation_result = unsafe { libc::prctl(libc::PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) };
    if activation_result != 0 {
        Err(Errno::last())
    } else {
        Ok(())
    }
}

/// If the program failed to load, return a `false`. There will be a value in
fn load(program: &[FilterInstruction]) -> Result<(), Errno> {
    let mut seccomp_active_guard = SECCOMP_ACTIVE.lock().unwrap();
    if !*seccomp_active_guard {
        let seccomp_activated = activate_seccomp();
        if seccomp_activated.is_err() {
            trace!("failed to activate seccomp");
            return seccomp_activated;
        }
        trace!("seccomp activated");
        *seccomp_active_guard = true;
    }
    drop(seccomp_active_guard);

    let program = FilterProgram {
        len: program
            .len()
            .try_into()
            .expect("program is smaller than 65535 instructions"),
        filter: program.as_ptr(),
    };
    let load_result = unsafe {
        libc::prctl(
            libc::PR_SET_SECCOMP,
            libc::SECCOMP_MODE_FILTER,
            &program as *const _,
        )
    };
    if load_result != 0 {
        Err(Errno::last())
    } else {
        Ok(())
    }
}

/// This is Rust for `struct sock_fprog`. It's kind of like a field-swapped slice, but the length
/// is a `u16`, not `usize`.
#[repr(C)]
struct FilterProgram {
    len: u16,
    filter: *const FilterInstruction,
}

/// This is Rust for `struct sock_filter`.
#[repr(C)]
struct FilterInstruction {
    code: u16,
    jt: u8, // "Jump true"
    jf: u8, // "Jump false"
    k: u32, // "Generic multiuse field". Typically a constant. K for konstant?
}

// All taken from <linux/bpf_common.h>
const BPF_LD: u16 = 0x00;
const BPF_LDX: u16 = 0x01;
const BPF_ST: u16 = 0x02;
const BPF_STX: u16 = 0x03;
const BPF_ALU: u16 = 0x04;
const BPF_JMP: u16 = 0x05;
const BPF_RET: u16 = 0x06;
const BPF_MISC: u16 = 0x07;

// Sizes, from <linux/bpf_common.h>
const BPF_W: u16 = 0x00;
const BPF_H: u16 = 0x08;
const BPF_B: u16 = 0x10;

/// An enum form of BPF-permitted memory operation sizes. This is just to prevent invalid sizes
/// from being provided when building filter instructions.
#[derive(Copy, Clone, Debug)]
#[repr(u16)]
pub enum MemOpSize {
    W = BPF_W,
    H = BPF_H,
    B = BPF_B,
}

// Access modes, from <linux/bpf_common.h>
const BPF_IMM: u16 = 0x10;
const BPF_ABS: u16 = 0x20;
const BPF_IND: u16 = 0x40;
const BPF_MEM: u16 = 0x60;
const BPF_LEN: u16 = 0x80;
const BPF_MSH: u16 = 0xa0;

// Jump conditions, from <linux/bpf_common.h>
const BPF_JA: u16 = 0x00;
const BPF_JEQ: u16 = 0x10;
const BPF_JGT: u16 = 0x20;
const BPF_JGE: u16 = 0x30;
const BPF_JSET: u16 = 0x40;

// Source, from <linux/bpf_common.h>
const BPF_K: u16 = 0x00;
const BPF_X: u16 = 0x08;

struct FilterCode;

impl FilterCode {
    pub fn cmp_eq_const() -> u16 {
        BPF_JMP + BPF_JEQ + BPF_K
    }
    pub fn ld_abs(size: MemOpSize) -> u16 {
        BPF_LD + (size as u16) + BPF_ABS
    }
    pub fn ret() -> u16 {
        BPF_RET + BPF_K
    }
}

const SECCOMP_RET_ERRNO: u32 = 0x00050000;
const SECCOMP_RET_TRACE: u32 = 0x7ff00000;
const SECCOMP_RET_ALLOW: u32 = 0x7fff0000;
const SECCOMP_RET_TRAP: u32 = 0x00030000;
const SECCOMP_RET_KILL_THREAD: u32 = 0x00000000;
const SECCOMP_RET_KILL_PROCESS: u32 = 0x80000000;

// helper functions to construct FilterInstruction. Note that these helpers may involve
// seccomp-specific details.
impl FilterInstruction {
    /// Rust for `BPF_STMT`. Not a macro, nor a const fn, unfortunately.
    pub fn statement(code: u16, k: u32) -> Self {
        FilterInstruction {
            code,
            jt: 0,
            jf: 0,
            k,
        }
    }
    /// Rust for `BPF_JUMP`. Not a macro, nor a const fn, unfortunately. `BPF_JUMP` swaps argument
    /// order from the struct layout too.
    pub fn jump(code: u16, k: u32, jt: u8, jf: u8) -> Self {
        FilterInstruction { code, jt, jf, k }
    }
    pub fn load_arg(n: u8) -> Self {
        assert!(n < 6);

        let args_offset: u32 = memoffset::offset_of!(SeccompData, args)
            .try_into()
            .expect("args array is a reasonably small offset");
        let selected_arg = args_offset
            .checked_add((8 * n) as u32)
            .expect("arg[n] is also a reasonably small offset");
        FilterInstruction::statement(FilterCode::ld_abs(MemOpSize::W), selected_arg)
    }
    pub fn load_nr() -> Self {
        let nr_offset: u32 = memoffset::offset_of!(SeccompData, nr)
            .try_into()
            .expect("syscall nr field is a reasonably small offset");
        FilterInstruction::statement(FilterCode::ld_abs(MemOpSize::W), nr_offset)
    }
    pub fn allow() -> Self {
        FilterInstruction::statement(FilterCode::ret(), SECCOMP_RET_ALLOW)
    }
    pub fn trap() -> Self {
        FilterInstruction::statement(FilterCode::ret(), SECCOMP_RET_TRAP)
    }
    pub fn kill_process() -> Self {
        FilterInstruction::statement(FilterCode::ret(), SECCOMP_RET_KILL_PROCESS)
    }
    pub fn errno(errno: u16) -> Self {
        FilterInstruction::statement(FilterCode::ret(), SECCOMP_RET_ERRNO | (errno as u32))
    }
}

/// This is Rust for `struct seccomp_data`, the struct of information a filter operates over.
/// Translated from <linux/seccomp.h>.
#[repr(C)]
struct SeccompData {
    nr: i32,                  // The system call number
    arch: u32, // system call convention as an AUDIT_ARCH_* value defined in <linux/audit.h>
    instruction_pointer: u64, // at the time of the system call
    args: [u64; 6], // up to 6 system call arguments always stored as 64-it values regardless of the architecture.
}

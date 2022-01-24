/* inspired by fastly/h2o:lib/common/seccomp.c @ 3edddbdb73debeedcfb78b4f4154eb9319be7d06 */
#define __unused __attribute__((unused))

#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_X86_64

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <linux/net.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <elf.h>

#include <asm/unistd.h>

#include <sysexits.h>
#include <errno.h>
#include <assert.h>
#include <err.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>  /* for offsetof */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "seccomp.h"

#if defined(__has_feature)
  #if __has_feature(address_sanitizer)
    #define IS_ASAN_BUILD 1
  #endif
#elif defined(__SANITIZE_ADDRESS__)
  #define IS_ASAN_BUILD 1
#endif

#if IS_ASAN_BUILD
#include <sys/ptrace.h>
#endif

static int seccomp_initialized;

#define SECCOMP_FILTER_FAIL SECCOMP_RET_TRAP

// https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/arch/x86/entry/syscalls/syscall_64.tbl?h=v5.4.143#n343
#if !defined(__NR_statx) && defined(__linux__) && defined(__x86_64__)
#define __NR_statx 332
#endif

static const struct sock_filter main_insns[] = {
    BPF_STMT(BPF_LD+BPF_W+BPF_ABS,
        offsetof(struct seccomp_data, arch)),
    BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SECCOMP_AUDIT_ARCH, 1, 0),
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_FILTER_FAIL),
    BPF_STMT(BPF_LD+BPF_W+BPF_ABS,
        offsetof(struct seccomp_data, nr)),

    SC_ALLOW(__NR_gettid),
    SC_ALLOW(__NR_sched_yield),
    SC_ALLOW(__NR_sched_getaffinity),
    SC_ALLOW(__NR_sched_getparam),
    SC_ALLOW(__NR_sched_getscheduler),
    SC_ALLOW(__NR_sched_setscheduler),
    SC_ALLOW(__NR_sigaltstack),
    SC_ALLOW(__NR_wait4),
    SC_ALLOW(__NR_getppid),
    SC_ALLOW(__NR_dup2),
    SC_ALLOW(__NR_fork),
#if IS_ASAN_BUILD
    SC_ALLOW_ARG(__NR_ptrace, 0, PTRACE_GETREGS /* 12 */),
    SC_ALLOW_ARG(__NR_ptrace, 0, PTRACE_ATTACH /* 16 */),
    SC_ALLOW_ARG(__NR_ptrace, 0, PTRACE_DETACH /* 17 */),
    SC_ALLOW(__NR_execve), // ASAN might use the llvm-symbolizer command
#endif

    SC_ALLOW(__NR_sched_setaffinity),
    SC_ALLOW(__NR_open),
    SC_ALLOW(__NR_openat),
    SC_ALLOW(__NR_getcwd),
    SC_ALLOW(__NR_fallocate),
    SC_ALLOW(__NR_unlink),
    SC_ALLOW(__NR_getdents),
    SC_ALLOW(__NR_setgid),
    SC_ALLOW(__NR_rename),

    SC_ALLOW(__NR_rt_sigaction),
    SC_ALLOW(__NR_rt_sigreturn),
    SC_ALLOW(__NR_lstat),
    SC_ALLOW(__NR_ioctl),
    SC_ALLOW(__NR_newfstatat),
    SC_ALLOW(__NR_sendmmsg),
    SC_ALLOW(__NR_poll),
    SC_ALLOW(__NR_stat),
    SC_ALLOW(__NR_fstat),
#ifdef __NR_statx
    SC_ALLOW(__NR_statx),
#endif
    SC_ALLOW(__NR_pread64),
    SC_ALLOW(__NR_getpeername),
    SC_ALLOW(__NR_epoll_wait),
    SC_ALLOW(__NR_epoll_ctl),
    SC_ALLOW(__NR_epoll_create),
    SC_ALLOW(__NR_epoll_create1),
    SC_ALLOW(__NR_eventfd2),
    SC_ALLOW(__NR_dup),
    SC_ALLOW(__NR_getdents64),

    /*
     * NB: we can probably revise this to only allow PF_UNIX sockets.
     */
    SC_ALLOW(__NR_socket),
    SC_ALLOW(__NR_lseek),
    SC_ALLOW(__NR_socketpair),
    SC_ALLOW(__NR_writev),
    SC_ALLOW(__NR_readv),
    SC_ALLOW(__NR_tgkill),
    SC_ALLOW(__NR_set_robust_list),
    SC_ALLOW(__NR_clone),
    SC_ALLOW(__NR_accept4),
    SC_ALLOW(__NR_pipe),
    SC_ALLOW(__NR_getsockopt),
    SC_ALLOW(__NR_setsockopt),
    SC_ALLOW(__NR_sendmsg),
    SC_ALLOW(__NR_recvmsg),
    SC_ALLOW(__NR_recvmmsg),
    SC_ALLOW(__NR_sendto),
    SC_ALLOW(__NR_recvfrom),
    SC_ALLOW(__NR_sysinfo),
    SC_ALLOW(__NR_fcntl),
    SC_ALLOW(__NR_prctl),
    /*
     * Dangerous NB:
     */
    SC_ALLOW(__NR_bind),
    SC_ALLOW(__NR_connect),
    SC_ALLOW(__NR_accept),
    SC_ALLOW(__NR_listen),
    SC_ALLOW(__NR_readlink),
    /*
     * Additional syscalls that were smoked out of the CI process.
     */
    SC_ALLOW(__NR_uname),
    SC_ALLOW(__NR_access),
    SC_ALLOW(__NR_setuid),
    SC_ALLOW(__NR_setgroups),
    SC_ALLOW(__NR_getsockname),
    SC_ALLOW(__NR_setpriority),
    SC_ALLOW(__NR_getpriority),

    SC_ALLOW(__NR_brk),
    SC_ALLOW(__NR_clock_gettime),
    SC_ALLOW(__NR_close),
    SC_ALLOW(__NR_exit),
    SC_ALLOW(__NR_exit_group),
    SC_ALLOW(__NR_futex),
    SC_ALLOW(__NR_geteuid),
    SC_ALLOW(__NR_getpgid),
    SC_ALLOW(__NR_getpid),
    SC_ALLOW(__NR_getrandom),
    SC_ALLOW(__NR_gettimeofday),
    SC_ALLOW(__NR_getuid),
    SC_ALLOW(__NR_madvise),
    SC_ALLOW(__NR_restart_syscall),
    SC_ALLOW(__NR_getrusage),
    /*
     * Permit mmap(2) and mprotect(2) operations, but do not allow h2o
     * to create/update mappings with PROT_EXEC. This will make it harder
     * to overwrite the various callbacks in h2o to point at something
     * exploit defined.
     */
    //SC_ALLOW_ARG_MASK(__NR_mmap, 3, PROT_READ|PROT_WRITE|PROT_NONE),
    SC_ALLOW(__NR_mmap),
    SC_ALLOW_ARG_MASK(__NR_mprotect, 2, PROT_READ|PROT_WRITE|PROT_NONE),
    SC_ALLOW(__NR_mremap),
    SC_ALLOW(__NR_munmap),
    SC_ALLOW(__NR_nanosleep),
    SC_ALLOW(__NR_clock_nanosleep),
    SC_ALLOW(__NR_read),
    SC_ALLOW(__NR_rt_sigprocmask),
    SC_ALLOW(__NR_select),
    SC_ALLOW(__NR_shutdown),
    SC_ALLOW(__NR_time),
    SC_ALLOW(__NR_write),
    SC_ALLOW(__NR_bpf),
    SC_ALLOW(__NR_inotify_init1),
    SC_ALLOW(__NR_inotify_add_watch),
    SC_ALLOW(__NR_inotify_rm_watch),
    SC_ALLOW(__NR_clock_getres),
    BPF_STMT(BPF_RET+BPF_K, SECCOMP_FILTER_FAIL),
};

static const struct sock_fprog basic_program = {
    .len = (sizeof(main_insns) / sizeof(main_insns[0])),
    .filter = (struct sock_filter *)main_insns,
};

static void sandbox_violation(__unused int signum, siginfo_t *info, __unused void *void_context)
{

    (void) fprintf(stdout,
        "%s: unexpected system call (arch:0x%x,syscall:%d @ %p)\n",
        __func__, info->si_arch, info->si_syscall, info->si_call_addr);
    abort();
}

static void seccomp_helper(void)
{
    struct sigaction act;
    sigset_t mask;

    memset(&act, 0, sizeof(act));
    sigemptyset(&mask);
    sigaddset(&mask, SIGSYS);
    act.sa_sigaction = &sandbox_violation;
    act.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSYS, &act, NULL) == -1) {
        err(1, "sigaction(SIGSYS) failed");
    }
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
        err(1, "sigprocmask(SIGSYS) failed");
    }
}

void seccomp_bind(void)
{
    const struct sock_fprog *fprog;

    if (!seccomp_initialized) {
        seccomp_helper();
        seccomp_initialized = 1;
    }
    fprog = &basic_program;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
        err(1, "prctl(PR_SET_NO_NEW_PRIVS)");
    }
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, fprog) == -1) {
        err(1, "prctl(PR_SET_SECCOMP)");
    }
    printf("[SECCOMP] security policy bound to thread\n");
}


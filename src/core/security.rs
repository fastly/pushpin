pub fn limit_permissions() {
    // For now all we do is set up seccomp if running on linux
    // Doesn't set up if no-seccomp for use with Devly. Seccomp's BPF filter crashes under Rosetta
    // on Apple Silicon because the CPU arch flips between x32/x64 modes.

    #[cfg(all(target_os = "linux", not(test), not(feature = "no-seccomp")))]
    crate::core::seccomp::install_seccomp_connect_filter()
}

mod ffi {
    use super::*;

    #[no_mangle]
    pub extern "C" fn security_limit_permissions() {
        limit_permissions();
    }
}

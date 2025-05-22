mod ffi {
    #[no_mangle]
    pub extern "C" fn security_limit_permissions() {
        // for now all we do is set up seccomp if running on linux

        #[cfg(all(target_os = "linux", not(test)))]
        crate::core::seccomp::install_seccomp_connect_filter()
    }
}

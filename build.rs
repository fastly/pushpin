use std::collections::HashMap;
use std::error::Error;
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::Path;

fn main() -> Result<(), Box<dyn Error>> {
    let conf = {
        let mut conf = HashMap::new();

        let f = fs::File::open("conf.pri")?;
        let reader = BufReader::new(f);

        const CONF_VARS: &[&str] = &["APP_VERSION", "CONFIGDIR", "LIBDIR", "QT_INSTALL_LIBS"];

        for line in reader.lines() {
            let line = line?;

            for name in CONF_VARS {
                if line.starts_with(name) {
                    let pos = line
                        .find('=')
                        .unwrap_or_else(|| panic!("no '=' character following var {}", name));
                    conf.insert(name.to_string(), line[(pos + 1)..].trim().to_string());
                    break;
                }
            }
        }

        conf
    };

    let app_version = conf.get("APP_VERSION").unwrap();
    let config_dir = conf.get("CONFIGDIR").unwrap();
    let lib_dir = conf.get("LIBDIR").unwrap();
    let qt_install_libs = conf.get("QT_INSTALL_LIBS").unwrap();

    let cpp_lib_dir = fs::canonicalize(Path::new("src/cpp")).unwrap();

    println!("cargo:rustc-env=APP_VERSION={}", app_version);
    println!("cargo:rustc-env=CONFIG_DIR={}", config_dir);
    println!("cargo:rustc-env=LIB_DIR={}", lib_dir);

    println!("cargo:rustc-link-search={}", cpp_lib_dir.display());

    #[cfg(target_os = "macos")]
    println!("cargo:rustc-link-search=framework={}", qt_install_libs);

    #[cfg(not(target_os = "macos"))]
    println!("cargo:rustc-link-search={}", qt_install_libs);

    println!("cargo:rerun-if-changed=conf.pri");

    Ok(())
}

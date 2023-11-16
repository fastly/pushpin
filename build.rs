use std::collections::HashMap;
use std::env;
use std::error::Error;
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() -> Result<(), Box<dyn Error>> {
    let conf = {
        let mut conf = HashMap::new();

        let f = fs::File::open("conf.pri")?;
        let reader = BufReader::new(f);

        const CONF_VARS: &[&str] = &[
            "APP_VERSION",
            "CONFIGDIR",
            "LIBDIR",
            "QT_INSTALL_LIBS",
            "QMAKE_PATH",
            "MAKETOOL",
        ];

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
    let qmake_path = fs::canonicalize(conf.get("QMAKE_PATH").unwrap())?;
    let maketool = fs::canonicalize(conf.get("MAKETOOL").unwrap())?;

    let root_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let cpp_src_dir = root_dir.join(Path::new("src/cpp"));
    let cpp_lib_dir = root_dir.join(Path::new("target/cpp"));

    for dir in ["moc", "obj", "test-moc", "test-obj", "test-work"] {
        fs::create_dir_all(cpp_lib_dir.join(Path::new(dir)))?;
    }

    if !cpp_src_dir.join("Makefile").try_exists()? {
        assert!(Command::new(qmake_path)
            .args(["-o", "Makefile", "cpp.pro"])
            .current_dir(&cpp_src_dir)
            .status()?
            .success());
    }

    assert!(Command::new(maketool)
        .current_dir(&cpp_src_dir)
        .status()?
        .success());

    println!("cargo:rustc-env=APP_VERSION={}", app_version);
    println!("cargo:rustc-env=CONFIG_DIR={}", config_dir);
    println!("cargo:rustc-env=LIB_DIR={}", lib_dir);

    println!("cargo:rustc-link-search={}", cpp_lib_dir.display());

    #[cfg(target_os = "macos")]
    println!("cargo:rustc-link-search=framework={}", qt_install_libs);

    #[cfg(not(target_os = "macos"))]
    println!("cargo:rustc-link-search={}", qt_install_libs);

    println!("cargo:rerun-if-changed=conf.pri");
    println!("cargo:rerun-if-changed=src");

    Ok(())
}

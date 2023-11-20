use std::collections::HashMap;
use std::env;
use std::error::Error;
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread;

fn check_version(
    pkg: &str,
    found: &str,
    expect_maj: u16,
    expect_min: u16,
) -> Result<(), Box<dyn Error>> {
    let parts: Vec<&str> = found.split('.').collect();

    if parts.len() < 2 {
        return Err(format!("unexpected {} version string: {}", pkg, found).into());
    }

    let (maj, min): (u16, u16) = match (parts[0].parse(), parts[1].parse()) {
        (Ok(maj), Ok(min)) => (maj, min),
        _ => return Err(format!("unexpected {} version string: {}", pkg, found).into()),
    };

    if maj < expect_maj || (maj == expect_maj && min < expect_min) {
        return Err(format!(
            "{} version >={}.{} required, found: {}",
            pkg, expect_maj, expect_min, found
        )
        .into());
    }

    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let qt_host_bins = {
        let pkg = "Qt5Core";

        let host_bins = pkg_config::get_variable(pkg, "host_bins")?;

        if host_bins.is_empty() {
            return Err(format!("pkg-config variable host_bins not found for {}", pkg).into());
        }

        PathBuf::from(host_bins)
    };

    let qmake_path = fs::canonicalize(qt_host_bins.join("qmake"))
        .map_err(|_| format!("qmake not found in {}", qt_host_bins.display()).to_string())?;

    let qt_version = {
        let output = Command::new(&qmake_path)
            .args(["-query", "QT_VERSION"])
            .output()?;
        assert!(output.status.success());

        String::from_utf8(output.stdout)?.trim().to_string()
    };

    check_version("qt", &qt_version, 5, 12)?;

    let qt_install_libs = {
        let output = Command::new(&qmake_path)
            .args(["-query", "QT_INSTALL_LIBS"])
            .output()?;
        assert!(output.status.success());

        let libs_dir = PathBuf::from(String::from_utf8(output.stdout)?.trim());

        fs::canonicalize(&libs_dir).map_err(|_| {
            format!("QT_INSTALL_LIBS dir {} not found", libs_dir.display()).to_string()
        })?
    };

    let conf = {
        let mut conf = HashMap::new();

        let f = fs::File::open("conf.pri")?;
        let reader = BufReader::new(f);

        const CONF_VARS: &[&str] = &["APP_VERSION", "CONFIGDIR", "LIBDIR", "MAKETOOL"];

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
    let maketool = fs::canonicalize(conf.get("MAKETOOL").unwrap())?;

    let root_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let cpp_src_dir = root_dir.join(Path::new("src/cpp"));
    let cpp_lib_dir = root_dir.join(Path::new("target/cpp"));

    for dir in ["moc", "obj", "test-moc", "test-obj", "test-work"] {
        fs::create_dir_all(cpp_lib_dir.join(Path::new(dir)))?;
    }

    if !cpp_src_dir.join("Makefile").try_exists()? {
        assert!(Command::new(&qmake_path)
            .args(["-o", "Makefile", "cpp.pro"])
            .current_dir(&cpp_src_dir)
            .status()?
            .success());
    }

    let proc_count = thread::available_parallelism().map_or(1, |x| x.get());

    assert!(Command::new(maketool)
        .args(["-j", &proc_count.to_string()])
        .current_dir(&cpp_src_dir)
        .status()?
        .success());

    println!("cargo:rustc-env=APP_VERSION={}", app_version);
    println!("cargo:rustc-env=CONFIG_DIR={}", config_dir);
    println!("cargo:rustc-env=LIB_DIR={}", lib_dir);

    println!("cargo:rustc-link-search={}", cpp_lib_dir.display());

    #[cfg(target_os = "macos")]
    println!(
        "cargo:rustc-link-search=framework={}",
        qt_install_libs.display()
    );

    #[cfg(not(target_os = "macos"))]
    println!("cargo:rustc-link-search={}", qt_install_libs.display());

    println!("cargo:rerun-if-changed=conf.pri");
    println!("cargo:rerun-if-changed=src");

    Ok(())
}

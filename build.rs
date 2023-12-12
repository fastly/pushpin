use std::collections::HashMap;
use std::error::Error;
use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{BufRead, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread;
use std::{env, io};
use time::macros::format_description;
use time::OffsetDateTime;

const DEFAULT_PREFIX: &str = "/usr/local";

fn get_version() -> String {
    let mut version = env!("CARGO_PKG_VERSION").to_string();

    if version.ends_with("-dev") {
        let format = format_description!("[year][month][day]");

        let date_str = OffsetDateTime::now_utc().format(&format).unwrap();

        version.push_str(&format!("-{}", date_str));
    }

    version
}

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

fn prefixed_vars(prefix: &str) -> HashMap<String, String> {
    let mut out = HashMap::new();

    out.insert("BINDIR".into(), format!("{}/bin", prefix));
    out.insert("CONFIGDIR".into(), format!("{}/etc", prefix));
    out.insert("LIBDIR".into(), format!("{}/lib", prefix));
    out.insert("LOGDIR".into(), "/var/log".into());
    out.insert("RUNDIR".into(), "/var/run".into());

    out
}

fn env_or_default(name: &str, defaults: &HashMap<String, String>) -> String {
    match env::var(name) {
        Ok(s) => s,
        Err(_) => defaults.get(name).unwrap().to_string(),
    }
}

fn write_test_config_h(dest: &Path, test_dir: &Path) -> Result<(), Box<dyn Error>> {
    let mut f = fs::File::create(dest)?;

    writeln!(&mut f, "#define TESTDIR \"{}\"", test_dir.display())?;

    Ok(())
}

fn write_cpp_conf_pri(
    dest: &Path,
    release: bool,
    include_paths: &[&Path],
) -> Result<(), Box<dyn Error>> {
    let mut f = fs::File::create(dest)?;

    writeln!(&mut f, "CONFIG -= debug_and_release")?;

    if release {
        writeln!(&mut f, "CONFIG += release")?;
    } else {
        writeln!(&mut f, "CONFIG += debug")?;
    }

    writeln!(&mut f)?;

    for path in include_paths {
        writeln!(&mut f, "INCLUDEPATH += {}", path.display())?;
    }

    Ok(())
}

fn write_postbuild_conf_pri(
    dest: &Path,
    bin_dir: &str,
    lib_dir: &str,
    config_dir: &str,
    run_dir: &str,
    log_dir: &str,
) -> Result<(), Box<dyn Error>> {
    let mut f = fs::File::create(dest)?;

    writeln!(&mut f, "BINDIR = {}", bin_dir)?;
    writeln!(&mut f, "LIBDIR = {}/pushpin", lib_dir)?;
    writeln!(&mut f, "CONFIGDIR = {}/pushpin", config_dir)?;
    writeln!(&mut f, "RUNDIR = {}/pushpin", run_dir)?;
    writeln!(&mut f, "LOGDIR = {}/pushpin", log_dir)?;

    Ok(())
}

fn get_boost_path() -> Result<PathBuf, Box<dyn Error>> {
    let possible_paths = vec!["/usr/local/include", "/usr/include"];
    let boost_version = "boost/version.hpp";

    for path in possible_paths {
        let path = PathBuf::from(path);
        let full_path = path.join(boost_version);
        if full_path.exists() {
            let file = File::open(full_path)?;
            let reader = io::BufReader::new(file);

            for line in reader.lines() {
                match line {
                    Ok(x) => {
                        if x.contains("#define BOOST_LIB_VERSION") {
                            let parts: Vec<&str> = x.split('"').collect();
                            if parts.len() >= 2 {
                                let version = parts[1].replace('_', ".");
                                check_version("boost", &version, 1, 71)?;
                            } else {
                                return Err("Error finding boost package verion".into());
                            }
                        }
                    }
                    Err(_) => {
                        return Err("Error finding boost package verion".into());
                    }
                };
            }
            return Ok(path);
        }
    }

    Err("No boost package found".into())
}

fn find_in_path(name: &str) -> Option<PathBuf> {
    for d in env::var("PATH").unwrap_or_default().split(':') {
        if d.is_empty() {
            continue;
        }

        let path = Path::new(d).join(name);
        if path.exists() {
            return Some(path);
        }
    }

    None
}

fn main() -> Result<(), Box<dyn Error>> {
    // for qt 6, check for qmake in path. for previous versions, use pkg-config
    let qmake_path = match find_in_path("qmake") {
        Some(p) => p,
        None => {
            let qt_host_bins = {
                let pkg = "Qt5Core";

                let host_bins = pkg_config::get_variable(pkg, "host_bins")?;

                if host_bins.is_empty() {
                    return Err(format!(
                        "qmake must be in PATH or pkg-config variable host_bins must exist for {}",
                        pkg
                    )
                    .into());
                }

                PathBuf::from(host_bins)
            };

            fs::canonicalize(qt_host_bins.join("qmake"))
                .map_err(|_| format!("qmake not found in {}", qt_host_bins.display()).to_string())?
        }
    };

    let qt_version = {
        let output = Command::new(&qmake_path)
            .args(["-query", "QT_VERSION"])
            .output()?;
        assert!(output.status.success());

        String::from_utf8(output.stdout)?.trim().to_string()
    };

    check_version("qt", &qt_version, 5, 12)?;

    let boost_path = get_boost_path()?;

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

    let default_vars = {
        let prefix = match env::var("PREFIX") {
            Ok(s) => Some(s),
            Err(env::VarError::NotPresent) => None,
            Err(env::VarError::NotUnicode(_)) => return Err("PREFIX not unicode".into()),
        };

        if let Some(prefix) = prefix {
            prefixed_vars(&prefix)
        } else {
            prefixed_vars(DEFAULT_PREFIX)
        }
    };

    let bin_dir = env_or_default("BINDIR", &default_vars);
    let config_dir = env_or_default("CONFIGDIR", &default_vars);
    let lib_dir = env_or_default("LIBDIR", &default_vars);
    let log_dir = env_or_default("LOGDIR", &default_vars);
    let run_dir = env_or_default("RUNDIR", &default_vars);

    let root_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);
    let profile = env::var("PROFILE")?;

    let cpp_src_dir = root_dir.join("src/cpp");
    let cpp_tests_src_dir = root_dir.join("src/cpp/tests");

    for dir in ["moc", "obj", "test-moc", "test-obj", "test-work"] {
        fs::create_dir_all(out_dir.join(dir))?;
    }

    let cpp_test_work_dir = out_dir.join("test-work");

    let mut include_paths = Vec::new();

    include_paths.push(out_dir.as_ref());

    if boost_path != Path::new("/usr/include") {
        include_paths.push(boost_path.as_ref());
    }

    write_test_config_h(&out_dir.join("test_config.h"), &cpp_test_work_dir)?;

    write_cpp_conf_pri(
        &out_dir.join("conf.pri"),
        profile == "release",
        &include_paths,
    )?;

    write_postbuild_conf_pri(
        &Path::new("postbuild").join("conf.pri"),
        &bin_dir,
        &lib_dir,
        &config_dir,
        &run_dir,
        &log_dir,
    )?;

    assert!(Command::new(&qmake_path)
        .args([
            OsStr::new("-o"),
            out_dir.join("Makefile").as_os_str(),
            cpp_src_dir.join("cpp.pro").as_os_str(),
        ])
        .status()?
        .success());

    assert!(Command::new(&qmake_path)
        .args([
            OsStr::new("-o"),
            out_dir.join("Makefile.test").as_os_str(),
            cpp_tests_src_dir.join("tests.pro").as_os_str(),
        ])
        .status()?
        .success());

    assert!(Command::new(&qmake_path)
        .args(["-o", "Makefile", "postbuild.pro"])
        .current_dir("postbuild")
        .status()?
        .success());

    let proc_count = thread::available_parallelism().map_or(1, |x| x.get());

    assert!(Command::new("make")
        .args(["-f", "Makefile"])
        .args(["-j", &proc_count.to_string()])
        .current_dir(&out_dir)
        .status()?
        .success());

    assert!(Command::new("make")
        .args(["-f", "Makefile.test"])
        .args(["-j", &proc_count.to_string()])
        .current_dir(&out_dir)
        .status()?
        .success());

    println!("cargo:rustc-env=APP_VERSION={}", get_version());
    println!("cargo:rustc-env=CONFIG_DIR={}/pushpin", config_dir);
    println!("cargo:rustc-env=LIB_DIR={}/pushpin", lib_dir);

    println!("cargo:rustc-link-search={}", out_dir.display());

    #[cfg(target_os = "macos")]
    println!(
        "cargo:rustc-link-search=framework={}",
        qt_install_libs.display()
    );

    #[cfg(not(target_os = "macos"))]
    println!("cargo:rustc-link-search={}", qt_install_libs.display());

    println!("cargo:rerun-if-env-changed=RELEASE");
    println!("cargo:rerun-if-env-changed=PREFIX");
    println!("cargo:rerun-if-env-changed=BINDIR");
    println!("cargo:rerun-if-env-changed=CONFIGDIR");
    println!("cargo:rerun-if-env-changed=LIBDIR");
    println!("cargo:rerun-if-env-changed=LOGDIR");
    println!("cargo:rerun-if-env-changed=RUNDIR");
    println!("cargo:rerun-if-changed=src");

    Ok(())
}

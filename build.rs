use std::collections::HashMap;
use std::env;
use std::error::Error;
use std::fs;
use std::io::{self, BufRead, BufReader, ErrorKind, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread;
use time::macros::format_description;
use time::OffsetDateTime;

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

fn read_conf_pri() -> Result<HashMap<String, String>, Box<dyn Error>> {
    let mut conf = HashMap::new();

    let f = fs::File::open("conf.pri")?;

    let reader = BufReader::new(f);

    const CONF_VARS: &[&str] = &["BINDIR", "CONFIGDIR", "LIBDIR", "LOGDIR", "RUNDIR"];

    for line in reader.lines() {
        let line = line?;

        for name in CONF_VARS {
            if line.starts_with(name) {
                let pos = match line.find('=') {
                    Some(pos) => pos,
                    None => return Err(format!("no '=' character following var {}", name).into()),
                };

                conf.insert(name.to_string(), line[(pos + 1)..].trim().to_string());
                break;
            }
        }
    }

    const SUFFIXED: &[&str] = &["CONFIGDIR", "LIBDIR", "LOGDIR", "RUNDIR"];

    for (k, v) in conf.iter_mut() {
        if SUFFIXED.contains(&k.as_str()) {
            match v.strip_suffix("/pushpin") {
                Some(s) => *v = s.to_string(),
                None => return Err(format!("var {} missing /pushpin suffix", k).into()),
            }
        }
    }

    Ok(conf)
}

fn env_or_default(name: &str, defaults: &HashMap<String, String>) -> String {
    match env::var(name) {
        Ok(s) => s,
        Err(_) => defaults.get(name).unwrap().to_string(),
    }
}

fn write_cpp_conf_pri(path: &Path, boost_include_path: &str) -> Result<(), Box<dyn Error>> {
    let mut f = fs::File::create(path)?;

    writeln!(&mut f)?;

    #[cfg(not(target_os = "linux"))]
    writeln!(&mut f, "INCLUDEPATH += {}", boost_include_path)?;

    Ok(())
}

fn write_postbuild_conf_pri(
    path: &Path,
    bin_dir: &str,
    lib_dir: &str,
    config_dir: &str,
    run_dir: &str,
    log_dir: &str,
) -> Result<(), Box<dyn Error>> {
    let mut f = fs::File::create(path)?;

    writeln!(&mut f, "BINDIR = {}", bin_dir)?;
    writeln!(&mut f, "LIBDIR = {}/pushpin", lib_dir)?;
    writeln!(&mut f, "CONFIGDIR = {}/pushpin", config_dir)?;
    writeln!(&mut f, "RUNDIR = {}/pushpin", run_dir)?;
    writeln!(&mut f, "LOGDIR = {}/pushpin", log_dir)?;

    Ok(())
}

fn find_boost_include_path() -> Option<String> {
    let possible_paths = vec![
        "/usr/local/include",
        "/usr/include",
        "/usr/local/Cellar/boost/",
    ];
    let boost_header = "boost/signals2.hpp";

    for path in possible_paths {
        let full_path = Path::new(path).join(boost_header);
        if full_path.exists() {
            return Some(path.to_string());
        }
    }

    None
}

fn check_boost_version() -> Result<bool, Box<dyn Error>> {
    let output = Command::new("bash")
    .args(["-c", "grep \"BOOST_LIB_VERSION\" $(find /usr/local/Cellar/boost/ /usr/include /usr/local/include -name 'version.hpp' 2> /dev/null | grep 'boost/version.hpp') | grep -v \"//\" | cut -d '\"' -f2 | sed 's/_/./g'"])
    .output()?;

    let version_str = String::from_utf8(output.stdout)?.trim().to_string();
    let version_parts: Vec<&str> = version_str.split('.').collect();

    if version_parts.len() < 2 {
        return Ok(false);
    }

    let major_version = version_parts[0].parse::<i32>()?;
    let minor_version = version_parts[1].parse::<i32>()?;

    Ok(major_version > 1 || (major_version == 1 && minor_version >= 71))
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

    let default_vars = {
        let prefix = match env::var("PREFIX") {
            Ok(s) => Some(s),
            Err(env::VarError::NotPresent) => None,
            Err(env::VarError::NotUnicode(_)) => return Err("PREFIX not unicode".into()),
        };

        let vars = if let Some(prefix) = prefix {
            Some(prefixed_vars(&prefix))
        } else {
            match read_conf_pri() {
                Ok(vars) => Some(vars),
                Err(e) => {
                    let e: Option<Box<dyn Error>> = match e.downcast::<io::Error>() {
                        Ok(e) if e.kind() == io::ErrorKind::NotFound => None,
                        Ok(e) => Some(e),
                        Err(e) => Some(e),
                    };

                    if let Some(e) = e {
                        return Err(format!("failed to read conf.pri: {}", e).into());
                    }

                    None
                }
            }
        };

        match vars {
            Some(vars) => vars,
            None => prefixed_vars("/usr/local"),
        }
    };

    let bin_dir = env_or_default("BINDIR", &default_vars);
    let config_dir = env_or_default("CONFIGDIR", &default_vars);
    let lib_dir = env_or_default("LIBDIR", &default_vars);
    let log_dir = env_or_default("LOGDIR", &default_vars);
    let run_dir = env_or_default("RUNDIR", &default_vars);

    let root_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let cpp_src_dir = root_dir.join(Path::new("src/cpp"));
    let cpp_lib_dir = root_dir.join(Path::new("target/cpp"));

    for dir in ["moc", "obj", "test-moc", "test-obj", "test-work"] {
        fs::create_dir_all(cpp_lib_dir.join(Path::new(dir)))?;
    }

    match check_boost_version() {
        Ok(true) => {
            let boost_include_path =
                find_boost_include_path().ok_or("Boost include path not found")?;
            write_cpp_conf_pri(
                &cpp_lib_dir.join(Path::new("conf.pri")),
                &boost_include_path,
            )?;
        }
        Ok(false) => {
            return Err(Box::new(io::Error::new(
                ErrorKind::Other,
                "Boost version is not sufficient.",
            )));
        }
        Err(_) => {
            return Err(Box::new(io::Error::new(
                ErrorKind::Other,
                "Error checking Boost version.",
            )));
        }
    }

    write_postbuild_conf_pri(
        &Path::new("target").join(Path::new("postbuild_conf.pri")),
        &bin_dir,
        &lib_dir,
        &config_dir,
        &run_dir,
        &log_dir,
    )?;

    if !cpp_src_dir.join("Makefile").try_exists()? {
        assert!(Command::new(&qmake_path)
            .args(["-o", "Makefile", "cpp.pro"])
            .current_dir(&cpp_src_dir)
            .status()?
            .success());
    }

    let proc_count = thread::available_parallelism().map_or(1, |x| x.get());

    assert!(Command::new("make")
        .args(["-j", &proc_count.to_string()])
        .current_dir(&cpp_src_dir)
        .status()?
        .success());

    println!("cargo:rustc-env=APP_VERSION={}", get_version());
    println!("cargo:rustc-env=CONFIG_DIR={}/pushpin", config_dir);
    println!("cargo:rustc-env=LIB_DIR={}/pushpin", lib_dir);

    println!("cargo:rustc-link-search={}", cpp_lib_dir.display());

    #[cfg(target_os = "macos")]
    println!(
        "cargo:rustc-link-search=framework={}",
        qt_install_libs.display()
    );

    #[cfg(not(target_os = "macos"))]
    println!("cargo:rustc-link-search={}", qt_install_libs.display());

    println!("cargo:rerun-if-env-changed=PREFIX");
    println!("cargo:rerun-if-env-changed=BINDIR");
    println!("cargo:rerun-if-env-changed=CONFIGDIR");
    println!("cargo:rerun-if-env-changed=LIBDIR");
    println!("cargo:rerun-if-env-changed=LOGDIR");
    println!("cargo:rerun-if-env-changed=RUNDIR");
    println!("cargo:rerun-if-changed=conf.pri");
    println!("cargo:rerun-if-changed=src");

    Ok(())
}

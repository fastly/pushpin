use std::collections::HashMap;
use std::env;
use std::error::Error;
use std::ffi::OsStr;
use std::fmt;
use std::fs::{self, File};
use std::io::{self, BufRead, Write};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Output, Stdio};
use std::str::FromStr;
use std::thread;
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

#[derive(Clone)]
struct LibVersion {
    maj: u16,
    min: u16,
    orig: String,
}

#[derive(Debug)]
struct ParseVersionError;

impl fmt::Display for ParseVersionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        Ok(write!(f, "failed to parse version")?)
    }
}

impl Error for ParseVersionError {}

impl FromStr for LibVersion {
    type Err = ParseVersionError;

    fn from_str(s: &str) -> Result<LibVersion, Self::Err> {
        let parts: Vec<&str> = s.split('.').collect();

        if parts.len() < 2 {
            return Err(ParseVersionError);
        }

        let (maj, min): (u16, u16) = match (parts[0].parse(), parts[1].parse()) {
            (Ok(maj), Ok(min)) => (maj, min),
            _ => return Err(ParseVersionError),
        };

        Ok(LibVersion {
            maj,
            min,
            orig: s.to_string(),
        })
    }
}

fn check_version(
    pkg: &str,
    found: LibVersion,
    expect_maj: u16,
    expect_min: u16,
) -> Result<(), Box<dyn Error>> {
    if found.maj < expect_maj || (found.maj == expect_maj && found.min < expect_min) {
        return Err(format!(
            "{} version >={}.{} required, found: {}",
            pkg, expect_maj, expect_min, found.orig,
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

fn write_cpp_conf_pri(
    dest: &Path,
    release: bool,
    include_paths: &[&Path],
    deny_warnings: bool,
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

    writeln!(&mut f)?;

    if deny_warnings {
        writeln!(&mut f, "QMAKE_CXXFLAGS += \"-Werror\"")?;
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

// returned vec size guaranteed >= 1
fn get_args_lossy(command: &mut Command) -> Vec<String> {
    let mut args = vec![command.get_program().to_string_lossy().into_owned()];

    for s in command.get_args() {
        args.push(s.to_string_lossy().into_owned());
    }

    args
}

// convert Result<Output> to Result<ExitStatus>, separating stdout
fn take_stdout(result: io::Result<Output>) -> (io::Result<ExitStatus>, Vec<u8>) {
    match result {
        Ok(output) => (Ok(output.status), output.stdout),
        Err(e) => (Err(e), Vec::new()),
    }
}

fn check_command_result(
    program: &str,
    result: io::Result<ExitStatus>,
) -> Result<(), Box<dyn Error>> {
    let status = match result {
        Ok(status) => status,
        Err(e) => return Err(format!("{} failed: {}", program, e).into()),
    };

    if !status.success() {
        return Err(format!("{} failed, {}", program, status).into());
    }

    Ok(())
}

fn check_command(command: &mut Command) -> Result<(), Box<dyn Error>> {
    let args = get_args_lossy(command);

    println!("{}", args.join(" "));

    check_command_result(&args[0], command.status())
}

fn check_command_capture_stdout(command: &mut Command) -> Result<Vec<u8>, Box<dyn Error>> {
    let args = get_args_lossy(command);

    println!("{}", args.join(" "));

    // don't capture stderr
    let command = command.stderr(Stdio::inherit());

    let (result, output) = take_stdout(command.output());
    check_command_result(&args[0], result)?;

    Ok(output)
}

fn check_qmake(qmake_path: &Path) -> Result<LibVersion, Box<dyn Error>> {
    let version: LibVersion = {
        let output =
            check_command_capture_stdout(Command::new(qmake_path).args(["-query", "QT_VERSION"]))?;

        let s = String::from_utf8(output)?;
        let s = s.trim();

        match s.parse() {
            Ok(v) => v,
            Err(_) => return Err(format!("unexpected qt version string: [{}]", s).into()),
        }
    };

    check_version("qt", version.clone(), 5, 12)?;

    Ok(version)
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

fn find_qmake() -> Result<(PathBuf, LibVersion), Box<dyn Error>> {
    let mut errors = Vec::new();

    // check for a usable qmake in PATH

    let names = &["qmake", "qmake6", "qmake5"];

    for name in names {
        if let Some(p) = find_in_path(name) {
            match check_qmake(&p) {
                Ok(version) => return Ok((p, version)),
                Err(e) => errors.push(format!("skipping {}: {}", p.display(), e)),
            }
        }
    }

    if errors.is_empty() {
        errors.push(format!("none of ({}) found in PATH", names.join(", ")));
    }

    // check pkg-config

    let pkg = "Qt5Core";

    match pkg_config::get_variable(pkg, "host_bins") {
        Ok(host_bins) if !host_bins.is_empty() => {
            let host_bins = PathBuf::from(host_bins);

            match fs::canonicalize(host_bins.join("qmake")) {
                Ok(p) => match check_qmake(&p) {
                    Ok(version) => return Ok((p, version)),
                    Err(e) => errors.push(format!("skipping {}: {}", p.display(), e)),
                },
                Err(e) => errors.push(format!("qmake not found in {}: {}", host_bins.display(), e)),
            }
        }
        Ok(_) => errors.push(format!(
            "pkg-config variable host_bins does not exist for {}",
            pkg
        )),
        Err(e) => errors.push(format!("pkg-config error for {}: {}", pkg, e)),
    }

    Err(format!("unable to find a usable qmake: {}", errors.join(", ")).into())
}

fn get_qmake() -> Result<(PathBuf, LibVersion), Box<dyn Error>> {
    match env::var("QMAKE") {
        Ok(s) => {
            let path = PathBuf::from(s);
            let version = check_qmake(&path)?;

            Ok((path, version))
        }
        Err(env::VarError::NotPresent) => find_qmake(),
        Err(env::VarError::NotUnicode(_)) => Err("QMAKE not unicode".into()),
    }
}

fn contains_file_prefix(dir: &Path, prefix: &str) -> Result<bool, io::Error> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;

        if entry.file_name().as_bytes().starts_with(prefix.as_bytes()) {
            return Ok(true);
        }
    }

    Ok(false)
}

fn get_qt_lib_prefix(lib_dir: &Path, version_maj: u16) -> Result<String, Box<dyn Error>> {
    let prefixes = if cfg!(target_os = "macos") {
        [format!("Qt{}", version_maj), "Qt".to_string()]
    } else {
        [format!("libQt{}", version_maj), "libQt".to_string()]
    };

    for prefix in &prefixes {
        if contains_file_prefix(lib_dir, prefix)? {
            return Ok(prefix.strip_prefix("lib").unwrap_or(prefix).to_string());
        }
    }

    Err(format!(
        "no files in {} beginning with any of: {}",
        lib_dir.display(),
        prefixes.join(", ")
    )
    .into())
}

fn find_boost_include_dir() -> Result<PathBuf, Box<dyn Error>> {
    let paths = ["/usr/local/include", "/usr/include"];
    let version_filename = "boost/version.hpp";

    for path in paths {
        let path = PathBuf::from(path);
        let full_path = path.join(version_filename);

        if !full_path.exists() {
            continue;
        }

        let file = File::open(&full_path)?;
        let reader = io::BufReader::new(file);

        let mut version_line = None;

        for line in reader.lines() {
            match line {
                Ok(s) if s.contains("#define BOOST_LIB_VERSION") => version_line = Some(s),
                Ok(_) => continue,
                Err(e) => {
                    return Err(format!("failed to read {}: {}", full_path.display(), e).into())
                }
            }
        }

        let version_line = match version_line {
            Some(s) => s,
            None => return Err(format!("version line not found in {}", full_path.display()).into()),
        };

        let parts: Vec<&str> = version_line.split('"').collect();

        if parts.len() < 2 {
            return Err(format!("failed to parse version line in {}", full_path.display()).into());
        }

        let version = parts[1].replace('_', ".");

        let version = match version.parse() {
            Ok(v) => v,
            Err(_) => return Err(format!("unexpected boost version string: {}", version).into()),
        };

        check_version("boost", version, 1, 71)?;

        return Ok(path);
    }

    Err(format!(
        "{} not found in any of: {}",
        version_filename,
        paths.join(", ")
    )
    .into())
}

fn contains_subslice<T: PartialEq>(haystack: &[T], needle: &[T]) -> bool {
    haystack.windows(needle.len()).any(|w| w == needle)
}

fn main() -> Result<(), Box<dyn Error>> {
    let (qmake_path, qt_version) = get_qmake()?;

    let qt_install_libs = {
        let output = check_command_capture_stdout(
            Command::new(&qmake_path).args(["-query", "QT_INSTALL_LIBS"]),
        )?;

        let libs_dir = PathBuf::from(String::from_utf8(output)?.trim());

        fs::canonicalize(&libs_dir)
            .map_err(|_| format!("QT_INSTALL_LIBS dir {} not found", libs_dir.display()))?
    };

    let qt_lib_prefix = get_qt_lib_prefix(&qt_install_libs, qt_version.maj)?;

    let boost_include_dir = match env::var("BOOST_INCLUDE_DIR") {
        Ok(s) => PathBuf::from(s),
        Err(env::VarError::NotPresent) => find_boost_include_dir()?,
        Err(env::VarError::NotUnicode(_)) => return Err("BOOST_INCLUDE_DIR not unicode".into()),
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

    let mut include_paths = Vec::new();

    include_paths.push(out_dir.as_ref());

    if boost_include_dir != Path::new("/usr/include") {
        include_paths.push(boost_include_dir.as_ref());
    }

    let deny_warnings = match env::var("CARGO_ENCODED_RUSTFLAGS") {
        Ok(s) => {
            let flags: Vec<&str> = s.split('\x1f').collect();

            contains_subslice(&flags, &["-D", "warnings"])
        }
        Err(env::VarError::NotPresent) => false,
        Err(env::VarError::NotUnicode(_)) => {
            return Err("CARGO_ENCODED_RUSTFLAGS not unicode".into())
        }
    };

    write_cpp_conf_pri(
        &out_dir.join("conf.pri"),
        profile == "release",
        &include_paths,
        deny_warnings,
    )?;

    write_postbuild_conf_pri(
        &Path::new("postbuild").join("conf.pri"),
        &bin_dir,
        &lib_dir,
        &config_dir,
        &run_dir,
        &log_dir,
    )?;

    check_command(Command::new(&qmake_path).args([
        OsStr::new("-o"),
        out_dir.join("Makefile").as_os_str(),
        cpp_src_dir.join("cpp.pro").as_os_str(),
    ]))?;

    check_command(Command::new(&qmake_path).args([
        OsStr::new("-o"),
        out_dir.join("Makefile.test").as_os_str(),
        cpp_tests_src_dir.join("tests.pro").as_os_str(),
    ]))?;

    check_command(
        Command::new(&qmake_path)
            .args(["-o", "Makefile", "postbuild.pro"])
            .current_dir("postbuild"),
    )?;

    let proc_count = thread::available_parallelism().map_or(1, |x| x.get());

    check_command(
        Command::new("make")
            .args(["-f", "Makefile"])
            .args(["-j", &proc_count.to_string()])
            .current_dir(&out_dir),
    )?;

    check_command(
        Command::new("make")
            .args(["-f", "Makefile.test"])
            .args(["-j", &proc_count.to_string()])
            .current_dir(&out_dir),
    )?;

    println!("cargo:rustc-env=APP_VERSION={}", get_version());
    println!("cargo:rustc-env=CONFIG_DIR={}/pushpin", config_dir);
    println!("cargo:rustc-env=LIB_DIR={}/pushpin", lib_dir);

    println!("cargo:rustc-cfg=qt_lib_prefix=\"{}\"", qt_lib_prefix);

    println!("cargo:rustc-link-search={}", out_dir.display());

    if cfg!(target_os = "macos") {
        println!(
            "cargo:rustc-link-search=framework={}",
            qt_install_libs.display()
        );
    } else {
        println!("cargo:rustc-link-search={}", qt_install_libs.display());
    }

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

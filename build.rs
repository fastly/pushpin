use std::error::Error;
use std::fs;
use std::io::{BufRead, BufReader};

fn main() -> Result<(), Box<dyn Error>> {
    let (app_version, config_dir) = {
        let f = fs::File::open("conf.pri")?;
        let reader = BufReader::new(f);

        let mut av = String::new();
        let mut cd = String::new();

        for line in reader.lines() {
            let line = line?;

            if line.starts_with("APP_VERSION =") {
                let pos = line.find('=').unwrap();

                av = line[(pos + 1)..].trim().into();
            } else if line.starts_with("CONFIGDIR =") {
                let pos = line.find('=').unwrap();

                cd = line[(pos + 1)..].trim().into();
            }
        }

        (av, cd)
    };

    println!("cargo:rustc-env=APP_VERSION={}", app_version);
    println!("cargo:rustc-env=CONFIG_DIR={}", config_dir);

    Ok(())
}

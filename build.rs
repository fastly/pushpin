use std::error::Error;
use std::fs;
use std::io::{BufRead, BufReader};

fn main() -> Result<(), Box<dyn Error>> {
    let app_version = {
        let f = fs::File::open("conf.pri")?;
        let reader = BufReader::new(f);

        let mut s = String::new();

        for line in reader.lines() {
            let line = line?;

            if line.starts_with("APP_VERSION =") {
                let pos = line.find("=").unwrap();

                s = (&line[(pos + 1)..]).trim().into();
            }
        }

        s
    };

    println!("cargo:rustc-env=APP_VERSION={}", app_version);

    Ok(())
}

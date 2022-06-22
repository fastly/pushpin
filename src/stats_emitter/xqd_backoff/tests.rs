use super::*;
use serde::{de, Deserialize, Deserializer};

#[derive(Deserialize, Debug)]
pub struct TestConfig {
    pub foo: u32,
    #[serde(deserialize_with = "backoff", default)]
    pub cypress_error_delay: Option<Backoff>,
}

fn backoff<'de, D>(input: D) -> Result<Option<Backoff>, D::Error>
where
    D: Deserializer<'de>,
{
    let opt: Option<BackoffBuilder> = Deserialize::deserialize(input)?;
    match opt {
        Some(b) => b
            .with_context("xqd-backoff-test")
            .init()
            .map(Into::into)
            .map_err(de::Error::custom),
        None => Ok(None),
    }
}

#[test]
fn test_deserialize_from_toml_absent() {
    let tc: TestConfig = toml::from_str(
        r#"
foo = 4
        "#,
    )
    .expect("parse");
    assert!(tc.cypress_error_delay.is_none());
}

#[test]
fn test_deserialize_from_toml_present() {
    let tc: TestConfig = toml::from_str(
        r#"
foo = 4
cypress_error_delay = { min_s = 10, max_s = 70, base_s = 10 }
        "#,
    )
    .expect("parse");
    let bc = tc.cypress_error_delay.expect("some");
    assert_eq!(bc.min_ms, 10_000);
    assert_eq!(bc.max_ms, 70_000);
    assert_eq!(bc.base_ms, 10_000);
}

#[test]
fn test_invalid_config_min_above_max() {
    let res = Backoff::build()
        .with_min_s(70.0)
        .with_max_s(10.0)
        .with_base_s(5.0)
        .with_context("xqd-backoff-test")
        .init();
    assert!(
        matches!(res, Err(Error::MaxNotAboveMin { .. })),
        "res = {:?}",
        res
    );
}

#[test]
fn test_invalid_config_out_of_bounds() {
    let res = Backoff::build()
        .with_min_s(10.0)
        .with_max_s((u32::MAX as f32) / 999.0)
        .with_base_s(5.0)
        .with_context("xqd-backoff-test")
        .init();
    assert!(
        matches!(res, Err(Error::OutOfBounds { .. })),
        "res = {:?}",
        res
    );
}

#[test]
fn test_almost_out_of_bounds() {
    let res = Backoff::build()
        .with_min_s(10.0)
        .with_max_s((u32::MAX / 1000) as f32)
        .with_base_s(5.0)
        .with_context("xqd-backoff-test")
        .init();
    assert!(res.is_ok(), "res = {:?}", res);
}

#[test]
fn test_invalid_config_base_out_of_bounds() {
    let res = Backoff::build()
        .with_min_s(10.0)
        .with_max_s(70.0)
        .with_base_s(-2.0)
        .with_context("xqd-backoff-test")
        .init();
    assert!(
        matches!(res, Err(Error::OutOfBounds { .. })),
        "res = {:?}",
        res
    );
}

#[tokio::test]
async fn test_bounds() {
    let mut b = Backoff::build()
        .with_min_s(10.0)
        .with_max_s(70.0)
        .with_base_s(5.0)
        .with_context("xqd-backoff-test")
        .init()
        .unwrap();
    for _ in 0..200 {
        let d = b.next().unwrap().time_s();
        assert!(d >= 10.0, "d = {}", d);
        assert!(d <= 70.0, "d = {}", d);
    }
}

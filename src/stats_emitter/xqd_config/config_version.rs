use serde::Deserialize;

#[derive(Clone, Debug, Deserialize)]
pub struct RawConfigVersion {
    pub sha: Option<String>,
    pub timestamp: Option<String>,
}

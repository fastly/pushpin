use crate::stats_emitter::errors::ZeroCount;
use crate::stats_emitter::heavenly::uuid::ServiceID;
use serde::{de, Deserialize, Deserializer, Serialize, Serializer};
use std::{
    borrow::Cow,
    collections::{HashMap, HashSet},
    fmt,
    hash::{Hash, Hasher},
    sync::Arc,
    time::SystemTime,
};

#[derive(Debug, Serialize, Deserialize)]
pub struct Message {
    pub(crate) datacenter: Arc<DataCenter>,
    pub(crate) server: Arc<Server>,
    pub(crate) emitter: Arc<Emitter>,
    pub(crate) schema: HashSet<Cow<'static, str>>,
    pub(crate) schema_name: Arc<SchemaName>,
    pub(crate) timestamp_ns: String,
    #[serde(deserialize_with = "from_seq", serialize_with = "to_seq")]
    pub(crate) services: HashMap<ServiceID, Service>,
}

fn from_seq<'de, D>(deserializer: D) -> Result<HashMap<ServiceID, Service>, D::Error>
where
    D: Deserializer<'de>,
{
    let vec = Vec::<Service>::deserialize(deserializer)?;
    Ok(vec.into_iter().map(|v| (v.id, v)).collect())
}

fn to_seq<S>(input: &HashMap<ServiceID, Service>, serializer: S) -> Result<S::Ok, S::Error>
where
    S: Serializer,
{
    serializer.collect_seq(input.values())
}

impl Message {
    pub fn new(
        schema_name: Arc<SchemaName>,
        datacenter: Arc<DataCenter>,
        server: Arc<Server>,
        emitter: Arc<Emitter>,
    ) -> Self {
        Self {
            datacenter,
            server,
            emitter,
            schema: HashSet::new(),
            schema_name,
            timestamp_ns: SystemTime::now()
                .duration_since(SystemTime::UNIX_EPOCH)
                .expect("We did not live 2^64 seconds past Thursday, January 1st, 1970, 00:00")
                .as_nanos()
                .to_string(),
            services: HashMap::new(),
        }
    }
}

#[derive(Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct DataCenter(String);

impl DataCenter {
    pub fn new(input: impl ToString) -> Self {
        Self(input.to_string())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct Server(String);

impl Server {
    pub fn new(input: impl ToString) -> Self {
        Self(input.to_string())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct Emitter(String);

impl Emitter {
    pub fn new(input: impl ToString) -> Self {
        Self(input.to_string())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct SchemaName(String);

impl SchemaName {
    pub fn new(input: impl ToString) -> Self {
        Self(input.to_string())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Debug, Deserialize, Serialize, Eq)]
pub struct Service {
    pub(crate) id: ServiceID,
    #[serde(deserialize_with = "string_to_u64", serialize_with = "u64_to_string")]
    pub(crate) counters: HashMap<Cow<'static, str>, u64>,
}

fn string_to_u64<'de, D>(deserializer: D) -> Result<HashMap<Cow<'static, str>, u64>, D::Error>
where
    D: Deserializer<'de>,
{
    HashMap::<Cow<'static, str>, String>::deserialize(deserializer)?
        .into_iter()
        .map(|(k, v)| Ok((k, v.parse().map_err(de::Error::custom)?)))
        .collect()
}

fn u64_to_string<S>(
    input: &HashMap<Cow<'static, str>, u64>,
    serializer: S,
) -> Result<S::Ok, S::Error>
where
    S: Serializer,
{
    serializer.collect_map(input.into_iter().map(|(k, v)| (k, v.to_string())))
}

impl PartialEq for Service {
    fn eq(&self, other: &Self) -> bool {
        self.id == other.id
    }
}

impl Hash for Service {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.id.hash(state);
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
/// What pipeline should this
/// [`MessageAggregator`][crate::message_aggregator::MessageAggregator] be sending messages to?
pub enum Pipeline {
    Billing,
    CustomMetrics,
}

impl fmt::Display for Pipeline {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::Billing => write!(f, "billing in xqd-stats-emitter"),
            Self::CustomMetrics => write!(f, "custom metrics in xqd-stats-emitter"),
        }
    }
}

/// Message sent to the aggregator that contains the UUID of the service, which
/// metric to increase, and by how much.
#[derive(Debug, Clone)]
pub struct ChannelMessage {
    pub(crate) id: ServiceID,
    pub(crate) metric: Cow<'static, str>,
    pub(crate) count: u64,
}

impl ChannelMessage {
    pub fn new(
        id: ServiceID,
        metric: impl Into<Cow<'static, str>>,
        count: u64,
    ) -> Result<Self, ZeroCount> {
        if count == 0 {
            return Err(ZeroCount);
        }
        Ok(Self {
            id,
            metric: metric.into(),
            count,
        })
    }
}

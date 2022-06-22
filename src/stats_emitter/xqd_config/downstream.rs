use serde::Deserialize;

#[derive(Clone, Debug, Deserialize)]
pub struct RawDownstreamConfig {
    pub listen: Vec<String>,
    pub listen_socket_chmod: Option<u32>,
    pub listen_socket_chgrp: Option<String>,
    pub service_id_header_name: Option<String>,
    pub monotonic_request_ids: Option<bool>,

    pub socket_read_timeout_s: Option<u32>,
    pub socket_write_timeout_s: Option<u32>,
}

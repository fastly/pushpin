use crate::stats_emitter::xqd_backoff::FixedBackoff;
use crate::stats_emitter::{
    data_types::{Message, SchemaName},
    errors::MessageSenderError,
    options::MessageSenderMode,
};
use hyper::{
    body::Bytes,
    client::{connect::Connection, Client},
    service::Service,
    Body, Request, StatusCode, Uri,
};
use hyper_openssl::HttpsConnector;
use openssl::ssl::{SslConnector, SslFiletype, SslMethod, SslVerifyMode};
use prometheus::{self, opts, register_int_counter, IntCounter};
use std::{error::Error, sync::Arc};
use tokio::{
    fs::OpenOptions,
    io::{AsyncRead, AsyncWrite, AsyncWriteExt, BufWriter},
    sync::mpsc::Receiver,
};

lazy_static::lazy_static! {
    static ref XQD_STATS_EMITTER_SUCCESSFUL_MESSAGES: IntCounter =
        register_int_counter!(opts!(
            "xqd_stats_emitter_successful_messages",
            "number of messages successfully sent by the stats emitter"
        )).unwrap();
}

#[ctor::ctor]
fn init() {
    lazy_static::initialize(&XQD_STATS_EMITTER_SUCCESSFUL_MESSAGES);
}

pub(crate) async fn message_sender<C, T>(
    connector: C,
    schema_name: Arc<SchemaName>,
    mut queue_rx: Receiver<Message>,
    mode: MessageSenderMode,
) -> Result<(), MessageSenderError>
where
    C: Service<Uri, Response = T> + Send + Sync + Clone + 'static,
    C::Error: Into<Box<dyn Error + Send + Sync>>,
    C::Future: Send + 'static,
    T: AsyncRead + AsyncWrite + Connection + Unpin + Send + Sync + 'static,
{
    match mode {
        // In this mode the message sender will send messages via http if mtls is not
        // set or https if it is set to the given url. This is the mode we use
        // in production to emit things like billing stats.
        MessageSenderMode::Json { url, mtls } => {
            let mut backoff = FixedBackoff::new(
                // These times are in milliseconds and were recommended compared to a
                // randomized backoff as we're not contending with 100s of xqd processes
                // to send stats and so if we have 3 or 4 send stats at the same time
                // this is okay and won't cause a thundering herd problem. These numbers
                // give us a small window to check if things are okay with increasing,
                // but not exponential, backoff and capping off at 5 seconds per try, so
                // that when things do recover we're not waiting hours to retry and
                // losing many messages along the way.
                [10, 10, 100, 100, 500, 500, 3000, 3000, 5000],
                schema_name.as_str(),
            );

            let tls_connector = if let Some(mtls) = mtls {
                let mut ssl = SslConnector::builder(SslMethod::tls_client())?;
                ssl.set_ca_file(mtls.ca_path)?;
                ssl.set_certificate_file(mtls.cert_path, SslFiletype::PEM)?;
                ssl.set_private_key_file(mtls.key_path, SslFiletype::PEM)?;
                ssl.set_verify(if mtls.dangerous_no_peer_verification {
                    SslVerifyMode::NONE
                } else {
                    SslVerifyMode::PEER
                });
                HttpsConnector::with_connector(connector, ssl)?
            } else {
                HttpsConnector::with_connector(
                    connector,
                    SslConnector::builder(SslMethod::tls_client())?,
                )?
            };

            let client = Client::builder().build::<_, Body>(tls_connector);

            loop {
                // Receive the next message from the `MessageAggregator`
                let message = next_message(&mut queue_rx).await;

                // Bytes is a cheaply cloneable reader over the data so that we
                // don't copy the entire message every time we need to try
                // sending a message if we need to use backoff
                let body = Bytes::from(serde_json::to_vec(&message).unwrap());

                // Loop attempting to send the message up to the stats pipeline.
                loop {
                    // Build a fresh request based on the body above.
                    let req = Request::post(&url).body(body.clone().into()).unwrap();

                    // Send our request to the emitter pipeline
                    match client.request(req).await {
                        // If we sent a message and got a 200, send the next message
                        Ok(resp) if resp.status() == StatusCode::OK => {
                            XQD_STATS_EMITTER_SUCCESSFUL_MESSAGES.inc();
                            break;
                        }
                        // While we have done everything possible to make sure that we are compliant with
                        // the spec and that nothing will go bad and are more likely to error by being
                        // unable to connect with the pipeline, we do need to handle this scenario. We do
                        // so by just resending the message on a best effort and hope that things on the
                        // otherside clear up. If it's because we're not compliant then a high error rate
                        // will alert us, but we won't crash the system. We just won't get paid or have
                        // metrics for customers.
                        Ok(resp) => tracing::error!(?resp, "received a non 200 response"),
                        Err(err) => {
                            tracing::error!(?err, "failed to connect with emitter pipeline")
                        }
                    };

                    // If we get here, the request was unsuccessful, so backoff before retrying.
                    backoff
                        .next()
                        .expect("FixedBackoff was not configured with a max retry")
                        .await;
                }

                // We've successfully processed a message, so reset the backoff for the next time around
                backoff.reset();
            }
        }
        // In this mode the message sender will write and append messages to a
        // given file only and will not send anything over the network. This is
        // mode is primarily used for integration testing to verify we are
        // sending the stats that we want to send.
        MessageSenderMode::DumpFile { dump_file } => {
            // Create a new dump file to append messages too
            let mut f = BufWriter::new(
                OpenOptions::new()
                    .append(true)
                    .create_new(true)
                    .open(&dump_file)
                    .await
                    .map_err(|err| {
                        tracing::error!(?err, "failed to create a new dump file");
                        err
                    })
                    .unwrap(),
            );
            loop {
                // Receive the next message from the `MessageAggregator`
                let message = next_message(&mut queue_rx).await;
                // Append the message to the file
                f.write_all(
                    &serde_json::to_vec(&message)
                        .map_err(|err| {
                            tracing::error!(
                                ?err,
                                "failed to serialize JSON bytes to write to dump file"
                            );
                            err
                        })
                        .unwrap(),
                )
                .await
                .map_err(|err| {
                    tracing::error!(?err, "failed to write JSON to dump file");
                    err
                })
                .unwrap();
                // Write a newline to the file so the next message is written to
                // the newline
                f.write_all(b"\n")
                    .await
                    .map_err(|err| {
                        tracing::error!(?err, "failed to write newline to dump file");
                        err
                    })
                    .unwrap();
                // Make sure to flush the changes to disk before looping again
                f.flush()
                    .await
                    .map_err(|err| {
                        tracing::error!(?err, "failed to flush changes for dump file to disk");
                        err
                    })
                    .unwrap();
                XQD_STATS_EMITTER_SUCCESSFUL_MESSAGES.inc();
            }
        }
    }
}

/// A convenience method for each mode to receive the next method
async fn next_message(queue_rx: &mut Receiver<Message>) -> Message {
    queue_rx
        .recv()
        .await
        .expect("Sender did not close the channel")
}

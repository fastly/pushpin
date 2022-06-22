use crate::stats_emitter::heavenly::uuid::ServiceID;
use crate::stats_emitter::{
    data_types::{ChannelMessage, DataCenter, Emitter, Message, SchemaName, Server, Service},
    errors::EmitterError,
    message_sender::message_sender,
    options::{AggregatorConfig, RawAggregatorConfig},
};
use hyper::{client::connect::Connection, service::Service as HyperService, Uri};
use prometheus::{self, opts, register_int_counter, IntCounter};
use std::{borrow::Cow, collections::HashMap, convert::TryInto, error::Error, mem, sync::Arc};
use tokio::{
    io::{AsyncRead, AsyncWrite},
    sync::mpsc::{channel, error::TrySendError, Receiver, Sender},
    time::{timeout_at, Duration, Instant},
};

lazy_static::lazy_static! {
    static ref XQD_STATS_EMITTER_DROPPED_MESSAGES: IntCounter =
        register_int_counter!(opts!(
            "xqd_stats_emitter_dropped_messages",
            "number of messages dropped by the stats emitter"
        )).unwrap();
}

#[ctor::ctor]
fn init() {
    lazy_static::initialize(&XQD_STATS_EMITTER_DROPPED_MESSAGES);
}

/// Type alias for the queue sender to a [`MessageAggregator`].
pub type MessageAggregatorSender = Sender<ChannelMessage>;
/// Type alias for the queue receiver for a [`MessageAggregator`]
pub type MessageAggregatorReceiver = Receiver<ChannelMessage>;

/// The amount of messages a [`MessageAggregator`] can hold while processing and
/// aggregating messages before sending them off as a message to be emitted to a
/// given URL
///
/// # Calculating the Number
/// This const is the number of messages we can hold, but how did we come up with the
/// calculation? Note some of these numbers may change and the number that is
/// reached here in the comment may be out of date. To get the actual number look at
/// the consts below to do the final calculation or look at the static assertion
/// that checks this const for equality with a number written in.
///
/// To begin our caclculation we wanted to make sure that we could handle traffic for
/// around 26,000 requests per second (though as of 10/19/21 xqd does not handle that
/// many requests). We then then multiply that by 10 to handle even more traffic than
/// what even varnish handles currently. Each [`ChannelMessage`] ends up being 64 bytes
/// in size and with that calculation we get:
///
/// (10 * 26000 req)/sec * 64 bytes/msg * (1 kb/ 1024 bytes) * (1 mb / 1024 kb) ≈ 15.87mb * 1 req/sec
///
/// We then take the number of billing stats in xqd and divide it by 2 and floor
/// it to get a likely number of stats used per request. This is an estimate here that
/// could be changed with more proper measurement overtime. This gives us:
///
/// (floor(39/2) msg) * (1 sec/req)  = 19 stats * 1 sec/req
///
/// Put this together by rounding up the first number and we get:
///
/// 15.87mb * 1 req/sec * 19 msg * (1 sec/req) ≈ 317.4mb
///
/// We use this number as the upper bound in size for the queue. This is also
/// assuming that that no messages are being processed in a given second for
/// whatever reason. This is the upper limit for a worst case scenario in terms
/// of memory size and we alert if we ever drop messages here. With that being
/// our upper bound in size for the queue in memory we need to translate that into
/// a number of messages so that we can use the value as an input to
/// [`channel`][tokio::sync::mpsc::channel]:
///
/// 317.4mb * 1024 kb/mb * 1024 bytes/kb * (1 msg/64 bytes) ≈ 5,200,282 messages
///
/// Using all the numbers in the equation without using the approximations we
/// get 4,940,000 messages for the queue which is our actual limit. With that in
/// mind we can now create our actual const calculation below
const AGGREGATOR_QUEUE_LIMIT: usize = {
    const TRAFFIC_MULTIPLIER: usize = 10;
    const NUMBER_OF_STATS: usize = 38;
    const REQS_PER_SEC: usize = 26_000;
    const MESSAGE_SIZE: usize = mem::size_of::<ChannelMessage>();

    // We can simplify equation by removing the conversions to megabytes. We do
    // however keep the MESSAGE_SIZE value here as that might change if
    // ChannelMessage does change in size.
    (TRAFFIC_MULTIPLIER * REQS_PER_SEC * MESSAGE_SIZE * (NUMBER_OF_STATS / 2)) / MESSAGE_SIZE
};

/// This SID is used to increase the count for the demo service which is
/// a service with the sum per stat for all stats in the message.
/// This is used by the stats team to do things like update the
/// number of requests being made on the network map. You can
/// look at this issue for more information:
/// https://github.com/fastly/ExecuteD/issues/1446
const DEMO: ServiceID = ServiceID::from_static("demo");

// This lets us see what AGGREGATOR_QUEUE_LIMIT's actual value is by making a
// compile time assertion and making sure that our floating point math was
// indeed working the way we expect it to be.
static_assertions::const_assert_eq!(AGGREGATOR_QUEUE_LIMIT, 4_940_000);

/// The main entrypoint type for the crate. It's what allows other threads/tasks
/// to send items into the queue for being sent out to the emitter pipeline by
/// aggregating messages on a per second basis.
pub struct MessageAggregator {
    /// The field used to send data to the emitter pipeline sending task
    queue: Sender<Message>,
    /// The datacenter this aggregator lives in. Used to cheaply make new
    /// messages.
    datacenter: Arc<DataCenter>,
    /// The server this aggregator lives in. Used to cheaply make new messages.
    server: Arc<Server>,
    /// The emitter this aggregator is apart of. Used to cheaply make new
    /// messages.
    emitter: Arc<Emitter>,
    /// The name of the pipeline messages are being emitted too
    schema_name: Arc<SchemaName>,
    /// The current message that's being aggregated before being sent to the
    /// queue
    current: Message,
    /// The receiver that the aggregator gets stats and metrics from
    rx: MessageAggregatorReceiver,
}
impl MessageAggregator {
    /// Recieve a message from the queue
    async fn recv(&mut self) {
        match self.rx.recv().await {
            Some(item) => {
                // Increment the metric for a given id and if the metric or the
                // service don't exist create them, then increment the count and
                // metric for the `demo` service as well.
                self.increment_metric(item.id, &item.metric, item.count);
                self.increment_metric(DEMO, &item.metric, item.count);

                // Now that we've added the metric add it to the schema of the
                // message which is just a list of all metrics in the outgoing
                // message regardless of service. We use a HashSet here so we
                // can just insert the value and not worry if it already exists.
                self.current.schema.insert(item.metric.into());
            }
            None => tracing::warn!("MessageAggregator channel to receive messages is closed, no more messages will be aggregated"),
        }
    }

    /// Put the current message into the queue
    fn enqueue(&mut self) {
        // Put a new Message in place to aggregate new stats and get the old one
        // to push onto the queue
        let message = mem::replace(
            &mut self.current,
            Message::new(
                self.schema_name.clone(),
                self.datacenter.clone(),
                self.server.clone(),
                self.emitter.clone(),
            ),
        );

        // There is nothing to send so don't put a blank message in the queue
        if message.services.len() == 0 {
            return;
        }

        // In the case where the queue is actually full we start to drop
        // messages as we don't want an unbounded amount of messages eating up
        // memory. We instead log that we're dropping the message so that we
        // can alert on it.
        if let Err(err) = self.queue.try_send(message) {
            match err {
                TrySendError::Full(_message) => {
                    tracing::error!(
                        // TODO: Is this message to verbose if the log already
                        // includes this info? Does it have this info?
                        "Message from datacenter {} and server {} and emitter {} for {} was dropped!",
                        self.datacenter.as_str(),
                        self.server.as_str(),
                        self.emitter.as_str(),
                        self.schema_name.as_str(),
                    );
                    XQD_STATS_EMITTER_DROPPED_MESSAGES.inc();
                }
                // This can only happen if the receiver explicitly calls close.
                // Seeing as we never will this error state should not be
                // reached, but in the off chance it is we want to panic as this
                // is an unrecoverable error.
                TrySendError::Closed(_message) => {
                    panic!("Queue for sending messages was closed when it should not have been")
                }
            }
        }
    }

    /// Spawns a new `MessageAggregator` to listen for messages from the
    /// `MessageAggregatorSender` returned by this function. It aggregates the messages
    /// on a per second basis before sending them to the queue of another task
    /// where they will be taken out of the queue and sent out of ExecuteD to
    /// the stats aggregator and handle any errors that arise.
    pub fn spawn<C, T>(
        opts: RawAggregatorConfig,
        connector: C,
    ) -> Result<MessageAggregatorSender, EmitterError>
    where
        C: HyperService<Uri, Response = T> + Send + Sync + Clone + 'static,
        C::Error: Into<Box<dyn Error + Send + Sync>>,
        C::Future: Send + 'static,
        T: AsyncRead + AsyncWrite + Connection + Unpin + Send + Sync + 'static,
    {
        let AggregatorConfig {
            emitter,
            datacenter,
            server,
            schema_name,
            queue_size,
            mode,
        } = opts.try_into()?;

        let current = Message::new(
            schema_name.clone(),
            datacenter.clone(),
            server.clone(),
            emitter.clone(),
        );
        let (message_aggregator_tx, rx) = channel(AGGREGATOR_QUEUE_LIMIT);
        let (queue, queue_rx) = channel(queue_size);
        let message_aggregator = Self {
            queue,
            datacenter,
            server,
            emitter,
            schema_name: schema_name.clone(),
            current,
            rx,
        };

        // Spawn the agregator onto the runtime
        tokio::spawn(message_aggregation(message_aggregator));

        // Spawn the JSON message sending queue onto the runtime
        tokio::spawn(message_sender(connector, schema_name, queue_rx, mode));

        // Return the transmitter to the aggregator to the callee
        Ok(message_aggregator_tx)
    }

    fn increment_metric(&mut self, id: ServiceID, metric: &Cow<'static, str>, count: u64) {
        self.current
            .services
            .entry(id)
            // If the [`Service`] does not exist then insert it
            .or_insert(Service {
                id,
                counters: HashMap::new(),
            })
            .counters
            .entry(metric.to_owned())
            // If the metric exists increase the count and if
            // not insert the count that was sent
            .and_modify(|e| *e += count)
            .or_insert(count);
    }
}

async fn message_aggregation(mut message_aggregator: MessageAggregator) {
    // We create a new deadline 1 second from now
    let mut deadline = Instant::now()
        .checked_add(Duration::new(1, 0))
        .expect("We did not live 2^64 seconds past Thursday, January 1st, 1970, 00:00");

    loop {
        // As long as we have not hit our 1 second limit keep receiving
        // messages and storing them to be enqueued. Note we need to check
        // everytime we loop through rather than using timeout_at to avoid
        // biasing towards the future with timeout_at does.
        while Instant::now() < deadline {
            if timeout_at(deadline, message_aggregator.recv())
                .await
                .is_err()
            {
                break;
            }
        }
        // We have hit our 1 second mark so send the message to our
        // sending queue task.
        message_aggregator.enqueue();
        // Update the deadline by 1 second from the previous one to avoid drift
        deadline = deadline
            .checked_add(Duration::new(1, 0))
            .expect("We did not live 2^64 seconds past Thursday, January 1st, 1970, 00:00");
    }
}

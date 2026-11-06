/*
 * Copyright (C) 2015-2022 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include "config.h"
#include "defercall.h"
#include "eventloop.h"
#include "executor.h"
#include "handlerargsdata.h"
#include "handlerengine.h"
#include "httpsession.h"
#include "httpsessionupdatemanager.h"
#include "log.h"
#include "processquit.h"
#include "pushpin/src/handler/settings.rs.h"
#include "reactor.h"
#include "rust/bindings.h"
#include "simplehttpserver.h"
#include "timer.h"
#include "wssession.h"
#include <QStringList>
#include <assert.h>
#include <unistd.h>

static QString toQString(const rust::String &s) {
    return QString::fromUtf8(s.data(), (qsizetype)s.size());
}

static QStringList toQStringList(const rust::Vec<rust::String> &v) {
    QStringList out;
    for (const rust::String &s : v)
        out += toQString(s);

    return out;
}

static int runLoop(const QString &logFile, const HandlerEngine::Configuration &config) {
    // Includes worst-case subscriptions and update registrations
    int timersPerSession = qMax(TIMERS_PER_HTTPSESSION, TIMERS_PER_WSSESSION) +
                           (config.connectionSubscriptionMax * TIMERS_PER_SUBSCRIPTION) +
                           TIMERS_PER_UNIQUE_UPDATE_REGISTRATION;

    // Enough timers for sessions, plus an extra 100 for misc
    int timersMax = (config.connectionsMax * timersPerSession) + 100;

    // Enough for control requests, plus an extra 100 for misc. Client sessions don't use socket
    // notifiers
    int socketNotifiersMax =
        (SOCKETNOTIFIERS_PER_SIMPLEHTTPREQUEST * CONTROL_CONNECTIONS_MAX) + 100;

    int registrationsMax = timersMax + socketNotifiersMax;

    Reactor reactor(registrationsMax);
    Executor executor(1); // For the one event loop task

    EventLoop *loop = nullptr;
    std::unique_ptr<DeferCall> deferCall;
    std::unique_ptr<HandlerEngine> engine;
    std::optional<int> exitCode;

    std::function<void()> setup = [&] {
        loop = EventLoop::instance();

        deferCall = std::make_unique<DeferCall>();

        deferCall->defer([&] {
            engine = std::make_unique<HandlerEngine>();

            ProcessQuit::instance()->quit.connect([&] {
                log_info("stopping...");

                // Remove the handler, so if we get another signal then we crash out
                ProcessQuit::cleanup();

                engine.reset();

                log_debug("stopped");

                loop->exit(0);
            });

            ProcessQuit::instance()->hup.connect([&] {
                log_info("reloading");
                log_rotate(logFile);
                engine->reload();
            });

            if (!engine->start(config)) {
                engine.reset();
                loop->exit(1);
                return;
            }

            log_info("started");
        });
    };

    std::function<void(int)> done = [&](int code) {
        deferCall.reset();

        exitCode = code;
    };

    auto fut = EventLoop::task(registrationsMax, &setup, &done);

    assert(Executor::currentSpawn(fut));

    assert(executor.run([&](std::optional<int> ms) { return reactor.poll(ms); }));

    return exitCode.value();
}

namespace ffi {

int handler_init(const HandlerCliArgs *argsFfi, const HandlerSettings &s) {
    HandlerArgsData args(argsFfi);

    HandlerEngine::Configuration config;
    config.appVersion = Config::get().version;
    config.instanceId = "handler_" + QByteArray::number(getpid());
    config.serverInStreamSpecs = toQStringList(s.server_in_stream_specs);
    config.serverOutSpecs = toQStringList(s.server_out_specs);
    config.clientOutSpecs = toQStringList(s.client_out_specs);
    config.clientOutStreamSpecs = toQStringList(s.client_out_stream_specs);
    config.clientInSpecs = toQStringList(s.client_in_specs);
    config.inspectSpecs = toQStringList(s.inspect_specs);
    config.acceptSpecs = toQStringList(s.accept_specs);
    config.retryOutSpecs = toQStringList(s.retry_out_specs);
    config.wsControlInitSpecs = toQStringList(s.ws_control_init_specs);
    config.wsControlStreamSpecs = toQStringList(s.ws_control_stream_specs);
    config.statsSpec = toQString(s.stats_spec);
    config.commandSpec = toQString(s.command_spec);
    config.stateSpec = toQString(s.state_spec);
    config.proxyStatsSpecs = toQStringList(s.proxy_stats_specs);
    config.proxyCommandSpec = toQString(s.proxy_command_spec);
    config.pushInSpec = toQString(s.push_in_spec);
    config.pushInSubSpecs = toQStringList(s.push_in_sub_specs);
    config.pushInSubConnect = s.push_in_sub_connect;
    config.pushInHttpAddr = QHostAddress(toQString(s.push_in_http_addr));
    config.pushInHttpPort = s.push_in_http_port;
    config.pushInHttpMaxHeadersSize = s.push_in_http_max_headers_size;
    config.pushInHttpMaxBodySize = s.push_in_http_max_body_size;
    config.ipcFileMode = s.ipc_file_mode;
    config.shareAll = s.share_all;
    config.messageRate = s.message_rate;
    config.messageHwm = s.message_hwm;
    config.messageBlockSize = s.message_block_size;
    config.messageWait = s.message_wait;
    config.idCacheTtl = s.id_cache_ttl;
    config.updateOnFirstSubscription = s.update_on_first_subscription;
    config.connectionsMax = s.connections_max;
    config.connectionSubscriptionMax = s.connection_subscription_max;
    config.subscriptionLinger = s.subscription_linger;
    config.statsConnectionSend = s.stats_connection_send;
    config.statsConnectionTtl = s.stats_connection_ttl;
    config.statsSubscriptionTtl = s.stats_subscription_ttl;
    config.statsReportInterval = s.stats_report_interval;
    config.statsFormat = toQString(s.stats_format);
    config.prometheusPort = toQString(s.prometheus_port);
    config.prometheusPrefix = toQString(s.prometheus_prefix);

    return runLoop(args.logFile, config);
}

} // namespace ffi

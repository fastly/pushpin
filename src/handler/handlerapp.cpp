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
#include "settings.h"
#include "simplehttpserver.h"
#include "timer.h"
#include "wssession.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <assert.h>
#include <iostream>
#include <unistd.h>

static void trimlist(QStringList *list) {
    for (int n = 0; n < list->count(); ++n) {
        if ((*list)[n].isEmpty()) {
            list->removeAt(n);
            --n; // Adjust position
        }
    }
}

static QStringList expandSpecs(const QStringList &l, int peerCount) {
    if (l.count() == 1 && l[0].startsWith("ipc:") && peerCount > 1) {
        QString base = l[0];

        QStringList out;
        for (int i = 0; i < peerCount; ++i)
            out += base + QString("-%1").arg(i);

        return out;
    }

    return l;
}

static QString firstSpec(const QString &s, int peerCount) {
    if (s.startsWith("ipc:") && peerCount > 1)
        return s + "-0";

    return s;
}

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

    // QSettings doesn't inform us if the config file can't be opened, so do that ourselves
    {
        QFile file(args.configFile);
        if (!file.open(QIODevice::ReadOnly)) {
            log_error("failed to open config file: %s", qPrintable(args.configFile));
            return 1;
        }
    }

    Settings settings(args.configFile);
    if (!args.ipcPrefix.isEmpty())
        settings.setIpcPrefix(args.ipcPrefix);
    if (args.portOffset != -1)
        settings.setPortOffset(args.portOffset);

    int proxyWorkerCount = settings.value("proxy/workers", 1).toInt();
    QStringList intreq_out_specs = settings.value("handler/proxy_intreq_out_specs").toStringList();
    trimlist(&intreq_out_specs);
    QStringList intreq_out_stream_specs =
        settings.value("handler/proxy_intreq_out_stream_specs").toStringList();
    trimlist(&intreq_out_stream_specs);
    QStringList intreq_in_specs = settings.value("handler/proxy_intreq_in_specs").toStringList();
    trimlist(&intreq_in_specs);
    QStringList proxy_inspect_specs = settings.value("handler/proxy_inspect_specs").toStringList();
    trimlist(&proxy_inspect_specs);
    QString proxy_inspect_spec = settings.value("handler/proxy_inspect_spec").toString();
    if (!proxy_inspect_spec.isEmpty())
        proxy_inspect_specs += proxy_inspect_spec;
    QStringList proxy_accept_specs = settings.value("handler/proxy_accept_specs").toStringList();
    trimlist(&proxy_accept_specs);
    QString proxy_accept_spec = settings.value("handler/proxy_accept_spec").toString();
    if (!proxy_accept_spec.isEmpty())
        proxy_accept_specs += proxy_accept_spec;
    QStringList proxy_retry_out_specs =
        settings.value("handler/proxy_retry_out_specs").toStringList();
    trimlist(&proxy_retry_out_specs);
    QString proxy_retry_out_spec = settings.value("handler/proxy_retry_out_spec").toString();
    if (!proxy_retry_out_spec.isEmpty())
        proxy_retry_out_specs += proxy_retry_out_spec;
    QStringList ws_control_init_specs =
        settings.value("handler/proxy_ws_control_init_specs").toStringList();
    trimlist(&ws_control_init_specs);
    QStringList ws_control_stream_specs =
        settings.value("handler/proxy_ws_control_stream_specs").toStringList();
    trimlist(&ws_control_stream_specs);
    QStringList proxy_stats_specs = settings.value("handler/proxy_stats_specs").toStringList();
    trimlist(&proxy_stats_specs);
    QString proxy_stats_spec = settings.value("handler/proxy_stats_spec").toString();
    if (!proxy_stats_spec.isEmpty())
        proxy_stats_specs += proxy_stats_spec;
    QString proxy_command_spec = settings.value("handler/proxy_command_spec").toString();

    if (proxy_inspect_specs.isEmpty() || proxy_accept_specs.isEmpty() ||
        proxy_retry_out_specs.isEmpty()) {
        log_error("must set proxy_inspect_specs, proxy_accept_specs, and "
                  "proxy_retry_out_specs");
        return 1;
    }

    std::unique_ptr<::DebugLogger> debugLogger;

    if (!s.debug_socket.empty()) {
        CowString debugError;
        debugLogger = ::DebugLogger::create(toQString(s.debug_socket), 1000, &debugError);
        if (!debugLogger) {
            log_error("failed to bind debug socket: %s", cowPrintable(debugError));
            return 1;
        }
    }

    HandlerEngine::Configuration config;
    config.appVersion = Config::get().version.asQString();
    config.instanceId = "handler_" + QByteArray::number(getpid());
    config.serverInStreamSpecs = toQStringList(s.server_in_stream_specs);
    config.serverOutSpecs = toQStringList(s.server_out_specs);
    config.clientOutSpecs = expandSpecs(intreq_out_specs, proxyWorkerCount);
    config.clientOutStreamSpecs = expandSpecs(intreq_out_stream_specs, proxyWorkerCount);
    config.clientInSpecs = expandSpecs(intreq_in_specs, proxyWorkerCount);
    config.inspectSpecs = expandSpecs(proxy_inspect_specs, proxyWorkerCount);
    config.acceptSpecs = expandSpecs(proxy_accept_specs, proxyWorkerCount);
    config.retryOutSpecs = expandSpecs(proxy_retry_out_specs, proxyWorkerCount);
    config.wsControlInitSpecs = expandSpecs(ws_control_init_specs, proxyWorkerCount);
    config.wsControlStreamSpecs = expandSpecs(ws_control_stream_specs, proxyWorkerCount);
    config.statsSpec = toQString(s.stats_spec);
    config.commandSpec = toQString(s.command_spec);
    config.stateSpec = toQString(s.state_spec);
    config.proxyStatsSpecs = expandSpecs(proxy_stats_specs, proxyWorkerCount);
    config.proxyCommandSpec = firstSpec(proxy_command_spec, proxyWorkerCount);
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

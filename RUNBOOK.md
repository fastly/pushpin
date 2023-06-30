# Pushpin

Pushpin is the core component of the Fanout and WebSockets products. For Fanout, it manages long-lived HTTP and WebSocket connections for pub/sub message delivery. For WebSockets, it manages passthrough WebSocket connections to the origin.

Pushpin receives client requests from H2O over a Unix socket.

Pushpin is a standard edge service, managed by Chef, configured with configly, and operated with systemctl and cachectl.

## Contact information

The Fanout team is responsible for Pushpin.

Slack: #fanout

## Code repositories

Code: [fanout/fst-pushpin](https://github.com/fastly/fst-pushpin).

Chef cookbooks:

* App cookbook for Fastly caches: [fastly-def/fst_app_h2o_pushpin](https://github.com/fastly-def/fst_app_h2o_pushpin).

Configly roles:

* [canary_fleet_5399_justinkarneges](https://github.com/fastly-def/configly-data/tree/master/_roles/canary_fleet_5399_justinkarneges). Canary role for use with the `releasectl canary` command.
* [pushpin_rc_autodeploy](https://github.com/fastly-def/configly-data/tree/master/_roles/pushpin_rc_autodeploy). This role may be used with the `releasectl rc` command.

## Log inspection

To view logs on a given cache node, login to the cache node and:

```
# Tail logs
journalctl -fu pushpin
journalctl -fu pushpin-proxy
journalctl -fu pushpin-handler
journalctl -fu pushpin-condure-in
journalctl -fu pushpin-condure-out

# View logs from a given point in time
journalctl -u pushpin --since '2022-04-03 14:00'

# View logs using systemctl
systemctl status pushpin

# View hardware logs
dmesg
```

Alternatively, view logs from [splunk](https://splunk-search.obs.gcp.secretcdn.net/splunk/en-US/app/launcher/home).

## On call troubleshooting

Background:

Pushpin does not directly coordinate with other Pushpin instances (pub/sub messages are distributed via Powderhorn), and Pushpin's health check only checks that the local Pushpin instance on a node is able to respond to requests without involving other nodes or origins. This means repairing the affected nodes should be enough to get health checks passing again, unless there's a traffic pattern triggering a bug that repeatedly takes down Pushpin.

Steps to follow:

1. If Pushpin is being reported as down (see #pushpin-alerts), investigate a node by checking the status and the logs of the 5 main services (pushpin, pushpin-proxy, pushpin-handler, pushpin-condure-in, pushpin-condure-out). Note any issues you are seeing (services not running, warnings, errors) and then restart the pushpin service (`sudo service pushpin restart`). This will in-turn restart the other services. Wait a few moments for the monitoring system to report Pushpin as up again.

2. If restarting Pushpin did not cause it to come back up, and it is down on many nodes (tens of nodes, or a large percentage of a single POP), try to determine if there is an external reason for the failure such as a separate incident involving other components. If there is no other easy explanation, and a new version of Pushpin was deployed in the past 48 hours, consider rolling back to the previous version.

3. If Pushpin is down on many nodes and there hasn't been a recent deploy and customers are complaining, page the Fanout team.

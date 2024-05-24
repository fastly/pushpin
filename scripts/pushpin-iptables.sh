#!/bin/bash

set -x

setup() {
        # Create the ipset if it does not exist because the services that
        # manage the ipsets may not yet have been created and started.
        ipset -q create proxyb_block_ingress hash:net -exist
        ipset -q create proxyb_block_ingress6 hash:net family inet6 -exist
        ipset -q create proxyb_block_egress hash:net -exist
        ipset -q create proxyb_block_egress6 hash:net family inet6 -exist

        iptables -N PUSHPINOUT
        iptables -I OUTPUT -m owner --uid-owner pushpin -g PUSHPINOUT
        iptables -I PUSHPINOUT -j ACCEPT
        iptables -I PUSHPINOUT -m set --match-set proxyb_block_egress dst -j DROP
        iptables -I PUSHPINOUT -m set --match-set proxyb_infra_blocked dst -j REJECT || echo >&2 "WARNING: Infra block rule skipped for IPv4"
        iptables -I PUSHPINOUT -p udp --dport 53 -s 127.0.0.1 -d 127.0.0.1  -j ACCEPT
        iptables -I PUSHPINOUT -p tcp --sport 1030 -s 127.0.0.1 -d 127.0.0.1 -j ACCEPT
        iptables -I PUSHPINOUT -p tcp --sport 1031 -s 127.0.0.1 -d 127.0.0.1 -j ACCEPT
        iptables -I PUSHPINOUT -p tcp --dport 9092 -s 127.0.0.1 -d 127.0.0.1 -j ACCEPT

        ip6tables -N PUSHPIN6OUT
        ip6tables -I OUTPUT -m owner --uid-owner pushpin -g PUSHPIN6OUT
        ip6tables -I PUSHPIN6OUT -j ACCEPT
        ip6tables -I PUSHPIN6OUT -m set --match-set proxyb_block_egress6 dst -j DROP
        ip6tables -I PUSHPIN6OUT -m set --match-set proxyb_infra_blocked6 dst -j REJECT || echo >&2 "WARNING: Infra block rule skipped for IPv6"
}

cleanup() {
        iptables -D OUTPUT -m owner --uid-owner pushpin -g PUSHPINOUT
        iptables -F PUSHPINOUT
        iptables -X PUSHPINOUT

        ip6tables -D OUTPUT -m owner --uid-owner pushpin -g PUSHPIN6OUT
        ip6tables -F PUSHPIN6OUT
        ip6tables -X PUSHPIN6OUT
}

if [ "$1" = "apply" ]; then
        cleanup
        set -e
        setup
elif [ "$1" = "rm" ]; then
        cleanup
        exit 0
else
        echo "Unsupported command '$1', please use 'apply' or 'rm'"
        exit 1
fi

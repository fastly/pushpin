#!/bin/sh

set -x -e

groupadd pushpin || true
groupadd pushpin-listener || true
groupadd pub-events || true
useradd -r -g pushpin pushpin || true
usermod -a -G pushpin-listener,pub-events pushpin || true
mkdir -p /var/run/pushpin
chown pushpin:pushpin /var/run/pushpin
chown pushpin:pushpin-listener /opt/fst-pushpin/bin/pushpin-healthcheck
chmod g+s /opt/fst-pushpin/bin/pushpin-healthcheck

cp /opt/fst-pushpin/etc/pushpin.service /etc/systemd/system
cp /opt/fst-pushpin/etc/pushpin-socat.service /etc/systemd/system
cp /opt/fst-pushpin/etc/pushpin-loader.service /etc/systemd/system

# Reload systemd so that it picks up the packaged unit fragment file.
# The query `systemctl is-system-running` exits with a non-0 status and it says
# "degrated" when one of the configured units failed.  That should not stop us.
status=$(systemctl is-system-running || true)
if [ "X$status" = Xrunning -o "X$status" = Xdegraded ]; then
  systemctl daemon-reload
  systemctl restart pushpin
  systemctl restart pushpin-loader
fi

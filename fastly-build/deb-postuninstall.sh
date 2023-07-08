#!/bin/sh

# Do not let systemctl problems prevent uninstallation.
set -x +e

rm -f /etc/systemd/system/pushpin.service
rm -f /etc/systemd/system/pushpin-condure-in.service
rm -f /etc/systemd/system/pushpin-condure-out.service
rm -f /etc/systemd/system/pushpin-proxy.service
rm -f /etc/systemd/system/pushpin-handler.service
rm -f /etc/systemd/system/pushpin-loader.service
rm -f /etc/systemd/system/pushpin-stats-emitter.service

# Reload systemd so that it picks up the removal of the packaged unit fragment file.
# The query `systemctl is-system-running` exits with a non-0 status and it says
# "degrated" when one of the configured units failed.  That should not stop us.
status=$(systemctl is-system-running || true)
if [ "X$status" = Xrunning -o "X$status" = Xdegraded ]; then
  systemctl daemon-reload
fi

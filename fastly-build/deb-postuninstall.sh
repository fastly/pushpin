#!/bin/sh

# Do not let systemctl problems prevent uninstallation.
set -x +e

rm -f /etc/systemd/system/pushpin.service

# Reload systemd so that it picks up the removal of the packaged unit fragment file.
# The query `systemctl is-system-running` exits with a non-0 status and it says
# "degrated" when one of the configured units failed.  That should not stop us.
status=$(systemctl is-system-running || true)
if [ "X$status" = Xrunning -o "X$status" = Xdegraded ]; then
  systemctl daemon-reload
fi

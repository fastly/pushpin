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

SECRETS="pushpin-nsq-client-cert.pem pushpin-nsq-client-key.pem pushpin-nsq-ca-cert.pem pushpin-sig-key.pem"

secrets_tmp_dir=/etc/pushpin/private/tmp
install -d -o root -g root -m 0700 "${secrets_tmp_dir}"

secrets_src_dir=/etc/vaultly/cache
on_secrets_fail=true

if [ -r /etc/chef/client.pem -a -x /opt/chef/embedded/bin/ruby ]; then
  fail() { echo >&2 "ERROR: chef-vault fetch failed and fallback files are unavailable."; exit 1; }
  on_secrets_fail=fail
  /opt/fst-executed/bin/get-from-chef-vault --vault secrets --item cache_public --dest "${secrets_tmp_dir}" $SECRETS && secrets_src_dir="${secrets_tmp_dir}" || echo >&2 'WARNING: chef-vault fetch failed.'
fi

install -T -o pushpin -g pushpin -m 0400 "${secrets_src_dir}"/pushpin-nsq-client-cert.pem /etc/pushpin/private/nsq-client-cert.pem || ${on_secrets_fail}
install -T -o pushpin -g pushpin -m 0400 "${secrets_src_dir}"/pushpin-nsq-client-key.pem  /etc/pushpin/private/nsq-client-key.pem  || ${on_secrets_fail}
install -T -o pushpin -g pushpin -m 0400 "${secrets_src_dir}"/pushpin-nsq-ca-cert.pem     /etc/pushpin/private/nsq-ca-cert.pem     || ${on_secrets_fail}
install -T -o pushpin -g pushpin -m 0400 "${secrets_src_dir}"/pushpin-sig-key.pem         /etc/pushpin/private/sig-key.pem         || ${on_secrets_fail}

rm -rf "${secrets_tmp_dir}"

cp /opt/fst-pushpin/etc/pushpin.service /etc/systemd/system
cp /opt/fst-pushpin/etc/pushpin-socat.service /etc/systemd/system
cp /opt/fst-pushpin/etc/pushpin-loader.service /etc/systemd/system
cp /opt/fst-pushpin/etc/pushpin-stats-emitter.service /etc/systemd/system

# Reload systemd so that it picks up the packaged unit fragment file.
# The query `systemctl is-system-running` exits with a non-0 status and it says
# "degrated" when one of the configured units failed.  That should not stop us.
status=$(systemctl is-system-running || true)
if [ "X$status" = Xrunning -o "X$status" = Xdegraded ]; then
  systemctl daemon-reload
  # Make the restart optional:
  # 1) If it fails, it's because the node is being installed
  # 2) cachectl will make sure that the service is restarted when the
  #    node is enabled
  systemctl restart pushpin || true
  systemctl restart pushpin-loader
  systemctl restart pushpin-stats-emitter
fi

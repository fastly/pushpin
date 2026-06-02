#!/bin/bash
set -e

if [ -w /etc/pushpin/pushpin.conf ]; then
	sed -i \
		-e 's/services=.*/services=condure,pushpin-proxy,pushpin-handler/' \
		-e 's/push_in_spec=.*/push_in_spec=tcp:\/\/\*:5560/' \
		-e 's/push_in_http_addr=.*/push_in_http_addr=0.0.0.0/' \
		-e 's/push_in_sub_specs=.*/push_in_sub_spec=tcp:\/\/\*:5562/' \
		-e 's/command_spec=.*/command_spec=tcp:\/\/\*:5563/' \
		-e 's/^http_port=.*/http_port=7999/' \
		/etc/pushpin/pushpin.conf
else
	echo "docker-entrypoint.sh: unable to write to /etc/pushpin/pushpin.conf, readonly"
fi

# Set routes with ${target} for backwards-compatibility.
if [ -v target ]; then
	echo "* ${target},over_http" > /etc/pushpin/routes
fi

exec "$@"

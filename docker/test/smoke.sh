#!/bin/bash
#
# Smoke test for a built Pushpin Docker image.
#
# This checks that the image actually runs Pushpin correctly, not just that it
# builds. It covers startup, the GRIP stream and publish path, and an outbound
# HTTPS connection that verifies the backend's certificate against the image's
# system trust store.
#
# Usage: ./docker/test/smoke.sh <image>
#   e.g. ./docker/test/smoke.sh fanout/pushpin:1.42.0-1

set -euo pipefail

if [ $# -ne 1 ]; then
	echo "usage: $0 <image>" >&2
	exit 2
fi

IMAGE="$1"

# A Fastly-served host on our own domain with a publicly trusted certificate.
# It has to be public, since the point is to verify against the system trust
# store, and we own it so the test doesn't depend on someone else's site.
#
# The path is the apt Release file for noble. Every apt install fetches it, so
# if it stops returning a 200 we have bigger problems than this test. Bump it
# to a newer release once noble goes end of life (May 2029).
HTTPS_BACKEND_HOST="packages.pushpin.org"
HTTPS_BACKEND_PATH="/deb/dists/pushpin-noble/Release"

# How long to wait for Pushpin to come up, or for a message to arrive.
WAIT_SECONDS=30

# Suffix container names with our PID so parallel runs don't collide.
DEFAULT_CONTAINER="pushpin-smoke-default-$$"
HTTPS_CONTAINER="pushpin-smoke-https-$$"

WORK_DIR="$(mktemp -d)"
STREAM_PID=""

cleanup() {
	local exit_code=$?

	if [ -n "$STREAM_PID" ]; then
		kill "$STREAM_PID" 2>/dev/null || true
	fi

	# Container logs are the most useful thing to have when this fails in CI.
	if [ $exit_code -ne 0 ]; then
		for container in "$DEFAULT_CONTAINER" "$HTTPS_CONTAINER"; do
			if docker container inspect "$container" >/dev/null 2>&1; then
				echo "--- logs for $container ---"
				docker logs "$container" 2>&1 || true
			fi
		done
	fi

	docker rm -f "$DEFAULT_CONTAINER" "$HTTPS_CONTAINER" >/dev/null 2>&1 || true
	rm -rf "$WORK_DIR"

	exit $exit_code
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

pass() {
	echo "ok: $*"
}

# Prints the host address that a container's port got published to. We publish
# to random ports on 127.0.0.1 so the test never fights with anything else
# running on the machine.
host_address() {
	docker port "$1" "$2/tcp" | head -n 1
}

# Retries a command once a second until it succeeds or we run out of time.
wait_for() {
	local description="$1"
	shift

	for _ in $(seq "$WAIT_SECONDS"); do
		if "$@" >/dev/null 2>&1; then
			return 0
		fi

		sleep 1
	done

	fail "timed out after ${WAIT_SECONDS}s waiting for $description"
}

body_contains() {
	curl -sS --max-time 5 "$1" | grep -q "$2"
}

file_contains() {
	grep -q "$2" "$1"
}

echo "smoke testing $IMAGE"
docker run --rm --entrypoint pushpin "$IMAGE" --version

# 1. Startup with the default config.
#
# The default routes file is "* test", which sends everything to Pushpin's
# built-in test handler, so we don't need a backend for these checks.

docker run -d --name "$DEFAULT_CONTAINER" \
	-p 127.0.0.1::7999 \
	-p 127.0.0.1::5561 \
	"$IMAGE" >/dev/null

PROXY_ADDRESS="$(host_address "$DEFAULT_CONTAINER" 7999)"
PUBLISH_ADDRESS="$(host_address "$DEFAULT_CONTAINER" 5561)"

wait_for "the test handler to respond" \
	body_contains "http://$PROXY_ADDRESS/" "Hello from the Pushpin test handler"
pass "pushpin started and the test handler responded"

# 2. GRIP stream and publish.
#
# Holding a stream open and publishing to it goes through connmgr, the proxy,
# the handler, the publish API, and the ZMQ sockets between them, so it's a
# good single check that the services are wired together correctly.

#
# The proxy starts answering before its connection to the handler is ready, so
# a stream opened right after startup can get a 500 even though "/" worked.
# Keep retrying until a stream actually opens.

STREAM_OUTPUT="$WORK_DIR/stream.out"

open_stream() {
	curl -sS -N --max-time "$WAIT_SECONDS" "http://$PROXY_ADDRESS/stream" >"$STREAM_OUTPUT" 2>&1 &
	STREAM_PID=$!

	# Give it a few seconds to open. A failed attempt exits right away, so we
	# stop waiting as soon as curl is gone.
	for _ in $(seq 10); do
		if file_contains "$STREAM_OUTPUT" "\[stream opened\]"; then
			return 0
		fi

		if ! kill -0 "$STREAM_PID" 2>/dev/null; then
			break
		fi

		sleep 0.5
	done

	kill "$STREAM_PID" 2>/dev/null || true
	STREAM_PID=""
	return 1
}

# Called directly, not through wait_for, because wait_for runs the command in
# a way that would hide STREAM_PID from the rest of the script.
stream_opened=false
for _ in $(seq "$WAIT_SECONDS"); do
	if open_stream; then
		stream_opened=true
		break
	fi

	sleep 1
done

if [ "$stream_opened" != true ]; then
	fail "timed out waiting for a stream to open"
fi

MESSAGE="smoke-test-$$"
curl -sS -f --max-time 5 \
	-H "Content-Type: application/json" \
	-d "{\"items\":[{\"channel\":\"test\",\"formats\":{\"http-stream\":{\"content\":\"$MESSAGE\n\"}}}]}" \
	"http://$PUBLISH_ADDRESS/publish" >/dev/null

wait_for "the published message to reach the stream" file_contains "$STREAM_OUTPUT" "$MESSAGE"
pass "published message reached the open stream"

kill "$STREAM_PID" 2>/dev/null || true
STREAM_PID=""

# 3. Outbound HTTPS with certificate verification.
#
# No "insecure" flag, so this only passes if the image has a working trust
# store. "host=" sets the Host header, since our request is to localhost.

ROUTES_FILE="$WORK_DIR/routes"
echo "* $HTTPS_BACKEND_HOST:443,ssl,host=$HTTPS_BACKEND_HOST" >"$ROUTES_FILE"

# The container runs as uid 1001, so it needs to be able to read the file.
chmod 644 "$ROUTES_FILE"

docker run -d --name "$HTTPS_CONTAINER" \
	-p 127.0.0.1::7999 \
	-v "$ROUTES_FILE:/etc/pushpin/routes:ro" \
	"$IMAGE" >/dev/null

HTTPS_PROXY_ADDRESS="$(host_address "$HTTPS_CONTAINER" 7999)"

https_backend_ok() {
	local status
	status="$(curl -sS -o /dev/null -w '%{http_code}' --max-time 10 "http://$HTTPS_PROXY_ADDRESS$HTTPS_BACKEND_PATH")"
	[ "$status" = "200" ]
}

# This goes out over the internet, so give it the same retry window as startup
# instead of failing on the first blip.
wait_for "a 200 from https://$HTTPS_BACKEND_HOST$HTTPS_BACKEND_PATH through pushpin" https_backend_ok
pass "proxied to https://$HTTPS_BACKEND_HOST with certificate verification"

echo "all smoke tests passed"

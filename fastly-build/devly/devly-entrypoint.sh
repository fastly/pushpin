#!/bin/bash
set -e

cd /pushpin

# only fetch + build if binary doesn't exist or source changed
PUSHPIN_BIN="target/release/pushpin"
INSTALLED_BIN="/opt/fst-pushpin/bin/pushpin"
if [ ! -f "$PUSHPIN_BIN" ] || [ "$(find src build.rs Makefile Cargo.toml Cargo.lock -newer "$PUSHPIN_BIN" 2>/dev/null | head -1)" ]; then
    # Resolve SSH agent socket — SSH_AUTH_SOCK may point to a directory containing
    # a symlink to the actual socket (colima virtiofs workaround)
    if [ -d "$SSH_AUTH_SOCK" ]; then
        REAL_SOCK=$(find "$SSH_AUTH_SOCK" -name "agent.*" -o -type s 2>/dev/null | head -1)
        if [ -n "$REAL_SOCK" ]; then
            export SSH_AUTH_SOCK="$REAL_SOCK"
        fi
    fi

    if [ -z "$SSH_AUTH_SOCK" ] || [ ! -e "$SSH_AUTH_SOCK" ]; then
        echo "ERROR: SSH agent socket not found. SSH agent forwarding is required for cargo fetch."
        echo "See: https://github.com/fastly/devly/wiki/Tutorial:-SSH-agent-forwarding"
        exit 1
    fi

    echo "Building pushpin..."
    cargo fetch
    make RELEASE=1 PREFIX=/opt/fst-pushpin CARGO_ARGS="--features no-seccomp" all install
    echo "Build complete."
elif [ ! -f "$INSTALLED_BIN" ]; then
    echo "Installing pushpin..."
    make RELEASE=1 PREFIX=/opt/fst-pushpin CARGO_ARGS="--features no-seccomp" install
    echo "Install complete."
else
    echo "Binary up to date, skipping build."
fi

mkdir -p /tmp/pushpin-run /tmp/pushpin-log /opt/fst-pushpin/etc/pushpin
cp fastly-build/devly/pushpin-devly.conf /opt/fst-pushpin/etc/pushpin/pushpin-devly.conf

exec /opt/fst-pushpin/bin/pushpin "$@"

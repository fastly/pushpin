FROM --platform=linux/amd64 ubuntu:20.04

ENV TZ=US \
    DEBIAN_FRONTEND=noninteractive

RUN apt-get -qq update && apt-get install -y zstd git pkg-config curl make g++ libssl-dev libzmq3-dev qtbase5-dev libboost-dev

# install toolchain
RUN curl https://sh.rustup.rs -sSf | \
    sh -s -- --default-toolchain stable-x86_64-unknown-linux-gnu -y

ENV RUSTUP_HOME="/root/.rustup" \
    CARGO_HOME="/root/.cargo" \
    PATH=/root/.cargo/bin:$PATH

# Keep in sync with minimum support Rust Version for Pushpin 
RUN rustup install 1.75.0-x86_64-unknown-linux-gnu

RUN cargo install cargo-audit

RUN rustup component add rustfmt

RUN rustup component add clippy

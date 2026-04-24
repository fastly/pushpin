FROM --platform=$BUILDPLATFORM ubuntu:24.04

ENV TZ=US \
    DEBIAN_FRONTEND=noninteractive

# apt repo for clang
RUN apt-get -qq update && apt-get install -y lsb-release software-properties-common gnupg curl
RUN curl https://apt.llvm.org/llvm-snapshot.gpg.key -sSf | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg
RUN echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/$(lsb_release -sc)/ llvm-toolchain-$(lsb_release -sc) main" | tee /etc/apt/sources.list.d/llvm.list
RUN apt-get -qq update

# install clang-format
RUN apt-get install -y clang-format-20
RUN update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-20 100

# install remaining dependencies
RUN apt-get install -y zstd git pkg-config make g++ libssl-dev libzmq3-dev qt6-base-dev libboost-dev black

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

FROM container-registry.secretcdn.net/fastly/base-focal:latest

ARG DESTDIR=/build
ARG SSH_AUTH_SOCK
ARG PKG_VERSION=unknown

WORKDIR /build
COPY . .

ENV DEBIAN_FRONTEND=noninteractive
ENV RUST_BACKTRACE=1

# set up debug packages repo
RUN apt-get update && apt-get -y install ubuntu-dbgsym-keyring
RUN printf "deb http://ddebs.ubuntu.com focal main restricted universe multiverse\ndeb http://ddebs.ubuntu.com focal-updates main restricted universe multiverse\ndeb http://ddebs.ubuntu.com focal-proposed main restricted universe multiverse" >/etc/apt/sources.list.d/ddebs.list

RUN apt-get update && apt-get -y install fst-ffpm=1.1-5 fst-stats=2.10.24-4010 build-essential coreutils libssl-dev python2.7 python3 patchelf gawk fst-gcc-9.1.0 qt5-default qt5-qmake libqt5core5a-dbgsym libqt5network5-dbgsym libglib2.0-0-dbgsym fst-rustc-1.75.0=1.75.0-206 fst-clang-8.0.1=1-43 strace pkg-config git fst-cmake libboost-dev

RUN mkdir ~/.ssh && \
  ssh-keyscan github.com >> ~/.ssh/known_hosts

RUN ls -alhrt
ENV CFLAGS="-fstack-protector-all -D_FORTIFY_SOURCE=2"
ENV CXXFLAGS="-fstack-protector-all -D_FORTIFY_SOURCE=2"
ENV LDFLAGS="-Wl,-z,now -Wl,-z,relro"
ENV CC=/opt/fst-gcc/9.1.0/bin/gcc
ENV CXX=/opt/fst-gcc/9.1.0/bin/g++
ENV RUST_TOOLCHAIN=/opt/fst-rust/1.75.0
ENV CLANG_TOOLCHAIN=/opt/fst-clang/8.0.1
ENV PATH="$PATH:$RUST_TOOLCHAIN/bin:$CLANG_TOOLCHAIN/bin"
ENV CARGO_NET_GIT_FETCH_WITH_CLI=true
RUN git clone ssh://git@github.com/zeromq/libzmq.git && cd libzmq && git checkout v4.3.4 && mkdir build && cd build && /opt/fst-cmake/bin/cmake .. && make -j $(nproc) && make DESTDIR=/ install
RUN cargo fetch && make RELEASE=1 PREFIX=/opt/fst-pushpin -j $(nproc)
RUN env LD_LIBRARY_PATH=/usr/local/lib make RELEASE=1 PREFIX=/opt/fst-pushpin check
RUN make RELEASE=1 PREFIX=/opt/fst-pushpin install
RUN cd pushpin-healthcheck && cargo build --release && cp ./target/release/pushpin-healthcheck /opt/fst-pushpin/bin
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin-legacy
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin-handler
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin-proxy
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin-publish
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin-stats-emitter
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin-connmgr
RUN env LD_LIBRARY_PATH=/usr/local/lib ./fastly-build/bundle_runtime_deps --stage=/ --libdir=/opt/fst-pushpin/lib /opt/fst-pushpin/bin/pushpin-healthcheck
RUN cp -a /usr/lib/debug /opt/fst-pushpin/lib
RUN cp ./fastly-build/packaging/pushpin-loader /opt/fst-pushpin/bin
RUN cp ./fastly-build/pushpin.conf /opt/fst-pushpin/etc/pushpin/pushpin.conf
RUN cp ./fastly-build/pushpin.service /opt/fst-pushpin/etc
RUN cp ./fastly-build/pushpin-connmgr-in.service /opt/fst-pushpin/etc
RUN cp ./fastly-build/pushpin-connmgr-out.service /opt/fst-pushpin/etc
RUN cp ./fastly-build/pushpin-proxy.service /opt/fst-pushpin/etc
RUN cp ./fastly-build/pushpin-handler.service /opt/fst-pushpin/etc
RUN cp ./fastly-build/pushpin-loader.service /opt/fst-pushpin/etc
RUN cp ./fastly-build/pushpin-stats-emitter.service /opt/fst-pushpin/etc
RUN cp ./scripts/pushpin-sandbox.sh /opt/fst-pushpin/bin
RUN cp ./scripts/pushpin-starter.sh /opt/fst-pushpin/bin
RUN cp ./scripts/pushpin-iptables.sh /opt/fst-pushpin/bin
RUN cp ./scripts/get-from-chef-vault /opt/fst-pushpin/bin
RUN /opt/fst-ffpm/bin/ffpm -s dir -t deb -n fst-pushpin --config-files /opt/fst-pushpin/etc/pushpin/routes --post-install ./fastly-build/deb-postinstall.sh --post-uninstall ./fastly-build/deb-postuninstall.sh -v $PKG_VERSION-$(/opt/fst-pushpin/bin/pushpin --version | awk '{printf "%s",$2;}') -p ${DESTDIR}/fst-pushpin-VERSION_ARCH.deb -C / opt/fst-pushpin

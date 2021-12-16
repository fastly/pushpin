FROM us.gcr.io/plat-elevation-preprod/fastly/base:latest

ARG DESTDIR=/build
ARG PKG_VERSION=unknown


WORKDIR /build
COPY . .

RUN apt-get update && apt-get -y install fst-ffpm=1.1-5 build-essential coreutils libssl-dev python2.7 python3 patchelf gawk fst-gcc-9.1.0 qt5-default qt5-qmake qconf fst-rustc-1.56.1=1.56.1-92 fst-clang-8.0.1=1-43 strace pkg-config git fst-cmake

RUN ls -alhrt
ENV CC=/opt/fst-gcc/9.1.0/bin/gcc
ENV CXX=/opt/fst-gcc/9.1.0/bin/g++
ENV RUST_TOOLCHAIN=/opt/fst-rust/1.56.1
ENV CLANG_TOOLCHAIN=/opt/fst-clang/8.0.1
ENV PATH="$PATH:$RUST_TOOLCHAIN/bin:$CLANG_TOOLCHAIN/bin"
RUN git clone https://github.com/zeromq/libzmq.git && cd libzmq && git checkout v4.3.4 && mkdir build && cd build && /opt/fst-cmake/bin/cmake .. && make -j $(nproc) && make DESTDIR=/ install
RUN git clone https://github.com/curl/curl.git && cd curl && git checkout curl-7_80_0 && mkdir build && cd build && /opt/fst-cmake/bin/cmake .. && make -j $(nproc) && make DESTDIR=/ install
RUN ./configure --prefix=/opt/fst-pushpin && make -j $(nproc) && make install
RUN cd condure && cargo build --release && cp ./target/release/condure /opt/fst-pushpin/bin
RUN cd zurl && ./configure --prefix=/opt/fst-pushpin && make -j $(nproc) && make install
RUN ./fastly-build/bundle_runtime_deps --stage=/opt/fst-pushpin/ --prefix=/opt/fst-pushpin /opt/fst-pushpin/bin/pushpin
RUN ./fastly-build/bundle_runtime_deps --stage=/opt/fst-pushpin/ --prefix=/opt/fst-pushpin /opt/fst-pushpin/bin/pushpin-handler
RUN ./fastly-build/bundle_runtime_deps --stage=/opt/fst-pushpin/ --prefix=/opt/fst-pushpin /opt/fst-pushpin/bin/pushpin-proxy
RUN ./fastly-build/bundle_runtime_deps --stage=/opt/fst-pushpin/ --prefix=/opt/fst-pushpin /opt/fst-pushpin/bin/pushpin-publish
RUN ./fastly-build/bundle_runtime_deps --stage=/opt/fst-pushpin/ --prefix=/opt/fst-pushpin /opt/fst-pushpin/bin/condure
RUN ./fastly-build/bundle_runtime_deps --stage=/opt/fst-pushpin/ --prefix=/opt/fst-pushpin /opt/fst-pushpin/bin/zurl
RUN /opt/fst-ffpm/bin/ffpm -s dir -t deb -n fst-pushpin -v $PKG_VERSION-$(/opt/fst-pushpin/bin/pushpin --version | awk '{printf "%s",$2;}') -p ${DESTDIR}/fst-pushpin-VERSION_ARCH.deb -C / opt/fst-pushpin

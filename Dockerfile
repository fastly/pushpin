FROM us.gcr.io/plat-elevation-preprod/fastly/base:latest

ARG DESTDIR=/build


WORKDIR /build
COPY . .

RUN apt-get update && apt-get -y install fst-ffpm=1.1-5 build-essential coreutils libssl-dev python2.7 python3 patchelf gawk fst-gcc-9.1.0 qt5-default qt5-qmake qconf libzmq1 libzmq-dev fst-rustc-1.56.1=1.56.1-92 fst-clang-8.0.1=1-43 strace

RUN ls -alhrt
ENV CC=/opt/fst-gcc/9.1.0/bin/gcc
ENV CXX=/opt/fst-gcc/9.1.0/bin/g++
ENV RUST_TOOLCHAIN=/opt/fst-rust/1.56.1
ENV CLANG_TOOLCHAIN=/opt/fst-clang/8.0.1
ENV PATH="$PATH:$RUST_TOOLCHAIN/bin:$CLANG_TOOLCHAIN/bin"
RUN strace -f -s 128 ./configure --prefix=/opt/fst-pushpin && make -j $(nproc) && make install
RUN cd cordure && cargo build --release && cp ./target/release/condure /opt/fst-pushpin/bin
RUN cd zurl && ./configure --prefix=/opt/fst-pushpin && make -j $(nproc) && make install
RUN ./fastly-build/bundle_runtime_deps --stage=/home/jenkins/workspace/teamc/fst-pushpin/ --prefix=/opt/fst-pushpin /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/bin/pushpin*
RUN ./fastly-build/bundle_runtime_deps --stage=/home/jenkins/workspace/teamc/fst-pushpin/ --prefix=/opt/fst-pushpin /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/bin/condure
RUN ./fastly-build/bundle_runtime_deps --stage=/home/jenkins/workspace/teamc/fst-pushpin/ --prefix=/opt/fst-pushpin /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/bin/zurl
RUN cp -r /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/ /opt
RUN /opt/fst-ffpm/bin/ffpm -s dir -t deb -n fst-pushpin -v $(/opt/fst-pushpin/bin/pushpin --version | awk '{printf "%s",$2;}')-$(cat fastly-build/VERSION) -p ${DESTDIR}/fst-pushpin-VERSION_ARCH.deb -C /home/jenkins/workspace/teamc/fst-pushpin/ opt/fst-pushpin

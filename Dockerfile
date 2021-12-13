FROM us.gcr.io/plat-elevation-preprod/fastly/base:latest

ARG DESTDIR=/build


WORKDIR /build
COPY . .

RUN apt-get update && apt-get -y install fst-ffpm=1.1-5 build-essential coreutils libssl-dev python2.7 python3 patchelf gawk fst-gcc-9.1.0

RUN ls -alhrt
ENV CC=/opt/fst-gcc/9.1.0/bin/gcc
ENV CXX=/opt/fst-gcc/9.1.0/bin/g++
RUN ./configure --prefix=/opt/fst-pushpin && make -j $(nproc) && make install
RUN cd cordure && cargo build --release && cp ./target/release/condure /opt/fst-pushpin/bin
RUN cd zurl && ./configure --prefix=/opt/fst-pushpin && make -j $(nproc) && make install
RUN ./fastly-build/bundle_runtime_deps --stage=/home/jenkins/workspace/teamc/fst-pushpin/ --prefix=/opt/fst-pushpin /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/bin/pushpin*
RUN ./fastly-build/bundle_runtime_deps --stage=/home/jenkins/workspace/teamc/fst-pushpin/ --prefix=/opt/fst-pushpin /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/bin/condure
RUN ./fastly-build/bundle_runtime_deps --stage=/home/jenkins/workspace/teamc/fst-pushpin/ --prefix=/opt/fst-pushpin /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/bin/zurl
RUN cp -r /home/jenkins/workspace/teamc/fst-pushpin/opt/fst-pushpin/ /opt
RUN /opt/fst-ffpm/bin/ffpm -s dir -t deb -n fst-pushpin -v $(/opt/fst-pushpin/bin/pushpin --version | awk '{printf "%s",$2;}')-$(cat fastly-build/VERSION) -p ${DESTDIR}/fst-pushpin-VERSION_ARCH.deb -C /home/jenkins/workspace/teamc/fst-pushpin/ opt/fst-pushpin

#
# Pushpin Dockerfile
#

# Pull the base image
FROM ubuntu:24.10 as build

# Install deps
RUN \
  apt-get update && \
  apt-get install -y bzip2 pkg-config make g++ rustc cargo libssl-dev qt6-base-dev libzmq3-dev libboost-dev

WORKDIR /build

ARG VERSION

ADD https://github.com/fastly/pushpin/releases/download/v${VERSION}/pushpin-${VERSION}.tar.bz2 .

RUN tar xf pushpin-${VERSION}.tar.bz2 && mv pushpin-${VERSION} pushpin

WORKDIR /build/pushpin

RUN make RELEASE=1 PREFIX=/usr CONFIGDIR=/etc
RUN make RELEASE=1 PREFIX=/usr CONFIGDIR=/etc check
RUN make RELEASE=1 PREFIX=/usr CONFIGDIR=/etc INSTALL_ROOT=/build/out install

FROM ubuntu:24.10
LABEL org.opencontainers.image.maintainer="Fastly OSS <oss@fastly.com>"

RUN \
  apt-get update && \
  apt-get install -y --no-install-recommends libqt6core6 libqt6network6 libzmq5 && \
  apt-get -y autoremove && \
  apt-get -y clean && \
  rm -rf /var/lib/apt/lists/*

COPY --from=build /build/out/ /

# Add entrypoint script
COPY docker-entrypoint.sh /usr/local/bin/

# Create a non-root user, group, directories and switch to that user.
RUN groupadd -r -g 1001 pushpin && \
    useradd -r -u 1001 -g pushpin pushpin

RUN mkdir -p /var/run/pushpin && \
    chown -R pushpin:pushpin /etc/pushpin /var/run/pushpin

# Using the user_id specifically here allows for less configuration in k8s
USER 1001

ENV LANG C.UTF-8

# Define default entrypoint and command
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["pushpin", "--merge-output"]

# Expose ports.
# - 7999: HTTP port to forward on to the app
# - 5560: ZMQ PULL for receiving messages
# - 5561: HTTP port for receiving messages and commands
# - 5562: ZMQ SUB for receiving messages
# - 5563: ZMQ REP for receiving commands
EXPOSE 7999
EXPOSE 5560
EXPOSE 5561
EXPOSE 5562
EXPOSE 5563

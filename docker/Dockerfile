#
# Pushpin Dockerfile
#
# https://github.com/fanout/docker-pushpin
#

# Pull the base image
FROM ubuntu:22.04

WORKDIR /pushpin-src

# Add private APT repository
RUN \
  apt-get update && \
  apt-get install -y pkg-config rustc qtbase5-dev libzmq3-dev \
  zurl

RUN \
  apt-get install -y condure build-essential

RUN \
  apt-get install -y cargo

# Cleanup
RUN \
  apt-get clean && \
  rm -fr /var/lib/apt/lists/* && \
  rm -fr /tmp/*


COPY . .


RUN ./configure 
RUN cargo build
RUN make
RUN make install

# Add entrypoint script
COPY docker/docker-entrypoint.sh /usr/local/bin/

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

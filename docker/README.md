# Pushpin Dockerfile


This repository contains **Dockerfile** of [Pushpin](http://pushpin.org/) for [Docker](https://www.docker.com/) published to the public [Docker Hub Registry](https://hub.docker.com/).

## Base Docker Image

* [ubuntu:24.10](https://hub.docker.com/_/ubuntu/)

## Installation

1. Install [Docker](https://www.docker.com/).

2. Download [automated build](https://hub.docker.com/r/fanout/pushpin/) from public [Docker Hub Registry](https://hub.docker.com/): `docker pull fanout/pushpin`

Alternatively, you can build an image from the `Dockerfile`. The easiest way to do this is with `docker-compose`, which picks up the source & image versions from `compose.yaml`:

```sh
docker-compose build
```

Or you can build with `docker` and manually specify the versions:

```sh
docker build --build-arg VERSION={SOURCE_VERSION} -t fanout/pushpin:{IMAGE_VERSION} .
```

## Usage

```sh
docker run \
  -d \
  -p 7999:7999 \
  -p 5560-5563:5560-5563 \
  --rm \
  --name pushpin \
  fanout/pushpin
```

By default, Pushpin routes traffic to a test handler.  See the [Getting Started Guide](https://pushpin.org/docs/getting-started/) for more information.

Open `http://<host>:7999` to see the result.

#### Configure Pushpin to route traffic

To add custom Pushpin configuration to your Docker container, attach a configuration volume.

```sh
docker run \
  -d \
  -p 7999:7999 \
  -p 5560-5563:5560-5563 \
  -v $(pwd)/config:/etc/pushpin/ \
  --rm \
  --name pushpin \
  fanout/pushpin
```

Note: The Docker entrypoint may make modifications to `pushpin.conf` so it runs properly in its container, exposing ports `7999`, `5560`, `5561`, `5562`, and `5563`.

See project documentation for more on [configuring Pushpin](https://pushpin.org/docs/configuration/).

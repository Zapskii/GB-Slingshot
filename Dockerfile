# GBDK-2020 toolchain for building GB-Slingshot.
# If GB-Protector already has a working Dockerfile, use that one instead;
# the Makefile only needs an image with `lcc` on PATH.
FROM ubuntu:24.04

ARG GBDK_VERSION=4.4.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl make python3 \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "https://github.com/gbdk-2020/gbdk-2020/releases/download/${GBDK_VERSION}/gbdk-linux64.tar.gz" \
    | tar -xz -C /opt

ENV GBDK_HOME=/opt/gbdk
ENV PATH="/opt/gbdk/bin:${PATH}"

WORKDIR /work

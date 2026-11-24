# Build both architectures from this single Dockerfile:
#   docker build --build-arg ARCH=aarch64 --target package -t bacnet_server-aarch64 .
#   docker build --build-arg ARCH=armv7hf --target package -t bacnet_server-armv7hf .
#
# Prefer ./build.sh, which does both and collects the .eap files in ./releases.
ARG ARCH=aarch64
ARG SDK_VERSION=12.10.0
ARG UBUNTU_VERSION=24.04
ARG SDK_REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${SDK_REPO}/${SDK}:${SDK_VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} AS package
ARG ARCH

# bacnet-stack is built unmodified as a static BACnet/IP library: no BBMD
# (see docs/PICS.md). Its Makefile hardcodes gcc, so pass the SDK toolchain.
# renovate: datasource=github-tags depName=bacnet-stack/bacnet-stack extractVersion=^bacnet-stack-(?<version>.+)$
ARG BACNET_STACK_VERSION=1.7.0
ARG BACNET_STACK_SHA256=9ebd5db0f1850be575849011e50c8a9750d2827a1e40cba89836c006d12e2629
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN mkdir -p /opt/bacnet-stack && \
    curl -fsSL -o /tmp/bacnet-stack.tar.gz \
        "https://github.com/bacnet-stack/bacnet-stack/archive/refs/tags/bacnet-stack-${BACNET_STACK_VERSION}.tar.gz" && \
    echo "${BACNET_STACK_SHA256}  /tmp/bacnet-stack.tar.gz" | sha256sum -c - && \
    tar -xzf /tmp/bacnet-stack.tar.gz --strip-components=1 -C /opt/bacnet-stack && \
    rm /tmp/bacnet-stack.tar.gz && \
    . /opt/axis/acapsdk/environment-setup* && \
    make -s -C /opt/bacnet-stack/apps lib BACDL=bip BBMD=none BACNET_PORT=linux CC="$CC" AR="$AR" >/dev/null

COPY app /opt/app/
COPY LICENSE /opt/app/LICENSE
WORKDIR /opt/app
RUN sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json && \
    find . -name .DS_Store -delete && \
    . /opt/axis/acapsdk/environment-setup* && acap-build ./ -a THIRD_PARTY_LICENSES.txt

FROM scratch
COPY --from=package /opt/app/*.eap /

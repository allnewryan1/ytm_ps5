# YouTube Music PS5 build image.
# Same pins EVO Player CI uses: Ubuntu 24.04, LLVM 18, Payload SDK v0.42,
# pacbrew-repo v0.39 (SDL2 + FFmpeg into the homebrew prefix).
ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION}

ARG LLVM_VERSION=18
ARG PS5_SDK_VERSION=v0.42
ARG PACBREW_VERSION=v0.39

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk \
    PS5_SDK_VERSION=${PS5_SDK_VERSION} \
    PACBREW_VERSION=${PACBREW_VERSION}

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash git wget ca-certificates \
    clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} \
    llvm-${LLVM_VERSION}-dev llvm-${LLVM_VERSION}-tools \
    make build-essential pkg-config \
    python3 \
    file binutils zip unzip tar xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-${LLVM_VERSION} 100 \
      --slave /usr/bin/clang++ clang++ /usr/bin/clang++-${LLVM_VERSION} \
 && update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-${LLVM_VERSION} 100 \
 && for t in ld.lld llvm-ar llvm-nm llvm-readelf llvm-objdump; do \
      update-alternatives --install "/usr/bin/$t" "$t" "/usr/bin/$t-${LLVM_VERSION}" 100; \
    done

COPY scripts/install-sdk-image.sh /usr/local/libexec/install-sdk-image.sh
COPY scripts/install-pacbrew-image.sh /usr/local/libexec/install-pacbrew-image.sh
RUN chmod +x /usr/local/libexec/install-sdk-image.sh /usr/local/libexec/install-pacbrew-image.sh \
 && /usr/local/libexec/install-sdk-image.sh \
 && /usr/local/libexec/install-pacbrew-image.sh

ENV PATH=/opt/ps5-payload-sdk/bin:/usr/local/bin:/usr/bin:/bin
WORKDIR /workspace
CMD ["/bin/bash"]

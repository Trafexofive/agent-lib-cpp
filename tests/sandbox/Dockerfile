# Agent sandbox — multi-stage Docker build
# Build:  docker build -t cortex-mk3-sandbox -f tests/sandbox/Dockerfile .
# Run:    docker run --rm --network none -e DEEPSEEK_API_KEY -v $(pwd):/workspace:ro cortex-mk3-sandbox --prompt "hello"

# ── Stage 1: Build ──
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make libcurl4-openssl-dev libjsoncpp-dev libcpp-httplib-dev libreadline-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN make clean && make cortex-mk3 LDFLAGS="-lcurl -ljsoncpp -lcpp-httplib -lreadline -lpthread" -j$(nproc)

# ── Stage 2: Runtime ──
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 libjsoncpp25 libcpp-httplib0.14t64 libreadline8 libgssapi-krb5-2 libkrb5-3 libssl3 \
    ca-certificates grep coreutils findutils \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -ms /bin/bash agent && \
    mkdir -p /workspace /tmp/agent && \
    chown agent:agent /tmp/agent

COPY --from=builder /build/cortex-mk3 /usr/local/bin/cortex-mk3
COPY manifests/ /opt/cortex/manifests/

USER agent
WORKDIR /workspace

ENTRYPOINT ["/usr/local/bin/cortex-mk3"]

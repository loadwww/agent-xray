FROM ubuntu:24.04

ARG AXR_VERSION=latest
ARG AXR_REPO=loadwww/agent-xray

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl libcurl4 \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    base="https://github.com/${AXR_REPO}/releases/latest/download"; \
    if [ "$AXR_VERSION" != "latest" ]; then base="https://github.com/${AXR_REPO}/releases/download/${AXR_VERSION}"; fi; \
    curl -fsSL --retry 2 "$base/axr-linux-x86_64.tar.gz" -o /tmp/axr.tar.gz; \
    curl -fsSL --retry 2 "$base/SHA256SUMS" -o /tmp/SHA256SUMS; \
    expected=$(awk '$2 == "axr-linux-x86_64.tar.gz" {print $1}' /tmp/SHA256SUMS); \
    test -n "$expected"; \
    echo "$expected  /tmp/axr.tar.gz" | sha256sum -c -; \
    tar -xzf /tmp/axr.tar.gz -C /usr/local/bin axr; \
    rm -f /tmp/axr.tar.gz /tmp/SHA256SUMS; \
    axr version

EXPOSE 8787

# 不设 ENTRYPOINT：默认跑离线演示，也可以 docker run <image> axr serve ...
# 容器里必须显式监听 0.0.0.0 并加 --allow-remote 才能从宿主机访问
CMD ["axr", "demo", "--no-open", "--port", "8787", "--bind", "0.0.0.0", "--allow-remote"]

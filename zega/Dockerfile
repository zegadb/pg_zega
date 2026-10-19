# syntax=docker/dockerfile:1
# Zega 0.2.0 (zegadb/zega@4ecaf43064c7320cd9d587fc68f21919e50ebc1d).
FROM rust:1.96.0-bookworm AS build
ARG ZEGA_REF=4ecaf43064c7320cd9d587fc68f21919e50ebc1d
RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git init \
    && git remote add origin https://github.com/zegadb/zega.git \
    && git fetch --depth 1 origin "$ZEGA_REF" \
    && git checkout --detach FETCH_HEAD
RUN cargo build --locked --release -p zega-cli \
    && install -m 0755 target/release/zega-server /zega-server

FROM debian:bookworm-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin --uid 10001 --user-group zega \
    && mkdir -p /var/lib/zega /etc/zega \
    && chown zega:zega /var/lib/zega \
    && touch /etc/zega/token \
    && chown zega:zega /etc/zega/token \
    && chmod 0600 /etc/zega/token
COPY --from=build /zega-server /usr/local/bin/zega-server
COPY entrypoint.sh /usr/local/bin/zega-entrypoint.sh
RUN chmod 0755 /usr/local/bin/zega-entrypoint.sh
EXPOSE 9342
ENTRYPOINT ["/usr/local/bin/zega-entrypoint.sh"]

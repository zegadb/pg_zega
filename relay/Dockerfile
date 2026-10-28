FROM rust:1.96.0-bookworm AS build
WORKDIR /build
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo build --locked --release

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/target/release/pg_zega-relay /usr/local/bin/pg_zega-relay
USER 65534:65534
ENTRYPOINT ["pg_zega-relay"]

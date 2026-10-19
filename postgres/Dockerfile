FROM postgres:17-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libcurl4 \
    libcurl4-openssl-dev \
    postgresql-server-dev-17 \
    && rm -rf /var/lib/apt/lists/*
COPY pg_zega.c pg_zega.control pg_zega--0.1.sql Makefile /build/pg_zega/
WORKDIR /build/pg_zega
RUN make && make install

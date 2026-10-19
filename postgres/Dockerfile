FROM postgres:17-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libcurl4-openssl-dev \
    postgresql-server-dev-17 \
    && rm -rf /var/lib/apt/lists/*
COPY pg_zega.c pg_zega.control pg_zega--0.1.sql Makefile /build/pg_zega/
WORKDIR /build/pg_zega
RUN make && make install

FROM postgres:17-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/lib/postgresql/17/lib/pg_zega.so /usr/lib/postgresql/17/lib/pg_zega.so
COPY --from=build /usr/share/postgresql/17/extension/pg_zega.control /usr/share/postgresql/17/extension/pg_zega.control
COPY --from=build /usr/share/postgresql/17/extension/pg_zega--0.1.sql /usr/share/postgresql/17/extension/pg_zega--0.1.sql
COPY entrypoint.sh /usr/local/bin/pg-zega-entrypoint.sh
COPY init-pg-zega.sh /docker-entrypoint-initdb.d/10-pg-zega.sh
RUN chmod 0755 /usr/local/bin/pg-zega-entrypoint.sh /docker-entrypoint-initdb.d/10-pg-zega.sh
ENTRYPOINT ["pg-zega-entrypoint.sh"]
CMD ["postgres"]

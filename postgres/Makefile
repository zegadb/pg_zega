MODULES = pg_zega
EXTENSION = pg_zega
DATA = pg_zega--0.1.sql
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# The MODULES pattern rule in Makefile.port does not append SHLIB_LINK.
pg_zega.so: pg_zega.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LDFLAGS_SL) -shared -o $@ -lcurl

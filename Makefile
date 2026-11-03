JANET_DIR  = vendor/janet

MODULE_big = pg_scalpel
EXTENSION  = pg_scalpel
DATA       = pg_scalpel--0.1.sql
OBJS       = pg_scalpel.o $(JANET_DIR)/build/c/janet.o
PG_CPPFLAGS  = -I$(JANET_DIR)/build
SHLIB_LINK = -lm

$(JANET_DIR)/build/c/janet.o: CFLAGS += -w

PG_CONFIG  = $(HOME)/pg19/install/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)



JANET_DIR    = vendor/janet
FUNCHOOK_DIR = vendor/funchook

MODULE_big = pg_scalpel
EXTENSION  = pg_scalpel
DATA       = pg_scalpel--0.1.sql
OBJS       = pg_scalpel.o pgs_timer.o pgs_sym_linux.o $(JANET_DIR)/build/c/janet.o
PG_CPPFLAGS  = -I$(JANET_DIR)/build

SHLIB_LINK = -lm -ldw -lelf -lffi

$(JANET_DIR)/build/c/janet.o: CFLAGS += -w

PG_CONFIG  = $(HOME)/pg19/install/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Build the vendored libraries: run "make vendor" once after a fresh
# "git clone --recurse-submodules".  Each step is skipped once its output exists.
.PHONY: vendor
vendor: $(JANET_DIR)/build/c/janet.c $(FUNCHOOK_DIR)/install/lib/libfunchook.a

$(JANET_DIR)/build/c/janet.c:
	$(MAKE) -C $(JANET_DIR)

$(FUNCHOOK_DIR)/install/lib/libfunchook.a:
	cmake -S $(FUNCHOOK_DIR) -B $(FUNCHOOK_DIR)/build -DCMAKE_BUILD_TYPE=Release \
	      -DFUNCHOOK_DISASM=capstone \
	      -DFUNCHOOK_BUILD_SHARED=OFF -DFUNCHOOK_BUILD_STATIC=ON \
	      -DFUNCHOOK_BUILD_TESTS=OFF \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_INSTALL_PREFIX=$(abspath $(FUNCHOOK_DIR))/install
	cmake --build $(FUNCHOOK_DIR)/build
	cmake --install $(FUNCHOOK_DIR)/build

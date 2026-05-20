MODULE_big = pg_tiktoken_c
OBJS = pg_tiktoken_c.o
EXTENSION = pg_tiktoken_c
DATA = sql/pg_tiktoken_c--1.0.sql \
       sql/pg_tiktoken_c--1.1.sql \
       sql/pg_tiktoken_c--1.0--1.1.sql
REGRESS = pg_tiktoken_c

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

# PCRE2 (required for Unicode-aware regex pre-tokenization)
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8 2>/dev/null)
PCRE2_LIBS   := $(shell pkg-config --libs   libpcre2-8 2>/dev/null || echo "-lpcre2-8")

ifeq ($(PCRE2_CFLAGS),)
  # Homebrew fallback on macOS arm64
  HOMEBREW_PREFIX := $(shell brew --prefix pcre2 2>/dev/null)
  ifneq ($(HOMEBREW_PREFIX),)
    PCRE2_CFLAGS := -I$(HOMEBREW_PREFIX)/include
    PCRE2_LIBS   := -L$(HOMEBREW_PREFIX)/lib -lpcre2-8
  endif
endif

override CFLAGS += $(PCRE2_CFLAGS)
SHLIB_LINK += $(PCRE2_LIBS)

# Vocab data directory (baked in at compile time)
TIKTOKEN_DATA_DIR := $(shell $(PG_CONFIG) --sharedir)/extension/pg_tiktoken_c
override CFLAGS += -DTIKTOKEN_DATA_DIR='"$(TIKTOKEN_DATA_DIR)"'

include $(PGXS)

# ---------------------------------------------------------------------------
# Download tiktoken vocab files (run once: make download-data)
# Files are installed to $(TIKTOKEN_DATA_DIR)/
# ---------------------------------------------------------------------------
TIKTOKEN_BASE_URL := https://openaipublic.blob.core.windows.net/encodings
TIKTOKEN_ENCS     := cl100k_base r50k_base p50k_base p50k_edit

download-data:
	@mkdir -p "$(TIKTOKEN_DATA_DIR)"
	@for enc in cl100k_base o200k_base r50k_base p50k_base; do \
	    dest="$(TIKTOKEN_DATA_DIR)/$$enc.tiktoken"; \
	    if [ ! -f "$$dest" ]; then \
	        echo "Downloading $$enc.tiktoken …"; \
	        curl -fsSL "$(TIKTOKEN_BASE_URL)/$$enc.tiktoken" -o "$$dest" || \
	            { echo "ERROR: failed to download $$enc.tiktoken"; exit 1; }; \
	    else \
	        echo "  $$enc.tiktoken already present, skipping."; \
	    fi \
	done
	@# p50k_edit shares the same BPE vocab as p50k_base (only special tokens differ)
	@if [ ! -f "$(TIKTOKEN_DATA_DIR)/p50k_edit.tiktoken" ]; then \
	    echo "  Copying p50k_base → p50k_edit (same BPE vocab)…"; \
	    cp "$(TIKTOKEN_DATA_DIR)/p50k_base.tiktoken" \
	       "$(TIKTOKEN_DATA_DIR)/p50k_edit.tiktoken"; \
	fi
	@echo "Done."

.PHONY: download-data

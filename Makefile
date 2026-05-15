CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinc
LDFLAGS = -lm

# library sources
LIB_SRC = $(wildcard src/codec2_mod.c) \
          $(wildcard src/analysis.c) \
          $(wildcard src/synthesis.c) \
          $(wildcard src/quantise.c) \
          $(wildcard src/interp.c) \
          $(wildcard src/lpc.c) \
          $(wildcard src/nlp.c) \
          $(wildcard src/delta_lsp_cb.c) \
          $(wildcard src/kiss_fft.c) \
          $(wildcard src/kiss_fftr.c) \
          $(wildcard src/util.c)

LIB_OBJ = $(LIB_SRC:.c=.o)

# tool sources
TOOLS = codec2_enc codec2_dec codec2_encodec

.PHONY: all clean tools

all: tools

tools: $(TOOLS)

# link each tool against the codec2 library objects
codec2_enc: src/codec2_enc.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

codec2_dec: src/codec2_dec.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

codec2_encodec: src/codec2_encodec.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# generic compile rule
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TOOLS) src/*.o

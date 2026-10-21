# tcalc - advanced terminal calculator
#
#   make              optimized build  ->  ./tcalc
#   make native       tuned for this CPU (-march=native)
#   make debug        -O0 -g with AddressSanitizer/UBSan
#   make test         run the test suite
#   make install      copy to $(PREFIX)/bin  (Termux sets PREFIX for you)
#   make uninstall / make clean

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
OPT     ?= -O2
CFLAGS  ?= $(OPT) -pipe
CFLAGS  += -std=c11 -Wall -Wextra -pedantic
LDLIBS  += -lm

BIN  = tcalc
SRC  = $(wildcard src/*.c)
OBJ  = $(SRC:.c=.o)
HDR  = src/calc.h src/term.h

.PHONY: all native debug test install uninstall clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

native: clean
	$(MAKE) OPT="-O3 -march=native"

debug: clean
	$(MAKE) OPT="-O0 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

test: $(BIN)
	sh tests/run_tests.sh ./$(BIN)

install: $(BIN)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	chmod 755 $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(OBJ) $(BIN)

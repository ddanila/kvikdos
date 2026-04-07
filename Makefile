.PHONY: all clean run test
.SUFFIXES:
MAKEFLAGS += -r

ALL = kvikdos guest.com slowp.com malloct.com mallocs.com printenv.com cat.com waitkey.com a20wrap.com vidtest.com

# -Werror=int-conversion: GCC 4.8.4 fails.
CFLAGS = -ansi -pedantic -s -O2 -W -Wall -Wextra -Werror=implicit-function-declaration -fno-strict-aliasing -Wno-overlength-strings $(XCFLAGS)
XCFLAGS =  # To be overridden from the command-line.

SRCDEPS = kvikdos.c mini_kvm.h
# On non-Linux (macOS), also compile the software 8086 CPU backend.
CPU8086_DEPS = cpu8086.c cpu8086.h cpu8086_xt.h mini_kvm.h XTulator/XTulator/cpu/cpu.c

all: $(ALL)

clean:
	rm -f $(ALL) kvikdos32 kvikdos64 kvikdos.static

run: kvikdos guest.com
	./kvikdos guest.com hello world

test: kvikdos guest.com cat.com printenv.com malloct.com a20wrap.com vidtest.com
	@echo "=== guest.com ===" && ./kvikdos guest.com hello world | grep -q "Hello, World" && echo "PASS" || { echo "FAIL"; exit 1; }
	@echo "=== cat.com ===" && echo "test123" | ./kvikdos cat.com | grep -q "test123" && echo "PASS" || { echo "FAIL"; exit 1; }
	@echo "=== printenv.com ===" && ./kvikdos printenv.com | grep -q "PATH=" && echo "PASS" || { echo "FAIL"; exit 1; }
	@echo "=== malloct.com ===" && ./kvikdos malloct.com | grep -q "malloct OK" && echo "PASS" || { echo "FAIL"; exit 1; }
	@echo "=== a20wrap.com ===" && ./kvikdos a20wrap.com | grep -q "A20 wrap OK" && echo "PASS" || { echo "FAIL"; exit 1; }
	@echo "=== vidtest.com ===" && ./kvikdos vidtest.com | grep -q "vidtest OK" && echo "PASS" || { echo "FAIL"; exit 1; }
	@echo "All tests passed."

a20wrap.com: a20wrap.nasm
	python3 -c 'from pathlib import Path; Path("a20wrap.com").write_bytes(bytes.fromhex("31 c0 8e d8 bb 31 40 30 c0 88 07 b8 00 ff 8e c0 bf 31 50 b0 5a aa 31 d2 8e da 38 07 75 0a 0e 1f ba 36 01 b4 09 cd 21 c3 0e 1f ba 44 01 b4 09 cd 21 b8 01 4c cd 21") + b"A20 wrap OK\r\n$$" + b"A20 wrap FAIL\r\n$$")'

%.com: %.nasm
	nasm -O0 -f bin -o $@ $<

ifeq ($(shell uname -s),Linux)
kvikdos: $(SRCDEPS)
	gcc $(CFLAGS) -o $@ $<
else
# cpu8086.c is adapted from XTulator (C99 code), so it needs relaxed flags.
CPU8086_CFLAGS = -O2 -W -Wall -Wextra -Werror=implicit-function-declaration -fno-strict-aliasing $(XCFLAGS)
kvikdos: $(SRCDEPS) $(CPU8086_DEPS)
	$(CC) $(CFLAGS) -c -o kvikdos.o kvikdos.c
	$(CC) $(CPU8086_CFLAGS) -c -o cpu8086.o cpu8086.c
	$(CC) -s -o $@ kvikdos.o cpu8086.o
	@rm -f kvikdos.o cpu8086.o
	@cp -f $@ kvikdos-soft
endif

kvikdos32: $(SRCDEPS)
	gcc -m32 -fno-pic -march=i686 -mtune=generic $(CFLAGS) -o $@ $<

kvikdos64: $(SRCDEPS)
	gcc -m64 -march=k8 -mtune=generic $(CFLAGS) -o $@ $<

kvikdos.static: $(SRCDEPS)
	xstatic gcc -m32 -fno-pic -D_FILE_OFFSET_BITS=64 -DUSE_MINI_KVM -march=i686 -mtune=generic $(CFLAGS) -o $@ $<

kvikdos.diet: $(SRCDEPS)
	minicc --gcc=4.8 --diet -DUSE_MINI_KVM -fno-strict-aliasing -o kvikdos.diet kvikdos.c

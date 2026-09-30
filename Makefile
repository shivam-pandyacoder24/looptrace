CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic
LDLIBS  ?= -lm

WASM_CC    ?= clang
WASM_FLAGS  = --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin \
              -Wl,--no-entry -Wl,-z,stack-size=1048576 -Wl,--strip-all

.PHONY: all test wasm web run clean

all: looptrace gen_data

looptrace: src/main.c src/fraud.c src/namemap.c src/fraud.h src/namemap.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/fraud.c src/namemap.c $(LDLIBS)

gen_data: tools/gen_data.c
	$(CC) $(CFLAGS) -o $@ $<

test_fraud: tests/test_fraud.c src/fraud.c src/fraud.h
	$(CC) $(CFLAGS) -o $@ tests/test_fraud.c src/fraud.c $(LDLIBS)

test: test_fraud
	./test_fraud

run: looptrace
	./looptrace data/sample.csv

# Same engine, compiled for the browser (needs clang + wasm-ld, no Emscripten)
wasm: web/looptrace.wasm

web/looptrace.wasm: src/fraud.c src/fraud.h src/wasm_api.c
	$(WASM_CC) $(WASM_FLAGS) -o $@ src/wasm_api.c src/fraud.c

# Bundle the website into docs/index.html (the folder you deploy)
#   make web AUTHOR="Your Name" LINK="https://github.com/you"
web:
	python3 tools/build_web.py $(if $(AUTHOR),--author "$(AUTHOR)") $(if $(LINK),--link "$(LINK)")

# Removes the native binaries. web/looptrace.wasm and docs/index.html are
# kept on purpose: they are committed so the site deploys without a compiler.
clean:
	rm -f looptrace gen_data test_fraud looptrace.exe gen_data.exe test_fraud.exe

# Riz — GNU Make (Linux/macOS; MSYS2 on Windows)
#   make          Build interpreter
#   make clean    Remove binary
#   make test     Smoke tests

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -std=c11 -O2 -Isrc
LDFLAGS ?= -lm
RM      ?= rm -f

SRC = src/diagnostic.c src/main.c src/lexer.c src/parser.c src/ast.c \
      src/interpreter.c src/value.c src/environment.c \
      src/chunk.c src/compiler.c src/vm.c src/codegen.c \
      src/riz_env.c src/riz_import.c src/pkg.c

OUT = riz

.PHONY: all clean run test plugin_llama

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)

clean:
	$(RM) $(OUT) plugin_llama_cli.so

run: $(OUT)
	./$(OUT)

test: $(OUT)
	./$(OUT) examples/hello.riz
	./$(OUT) --vm examples/vm_test.riz

# Optional: llama.cpp bridge (Linux .so)
plugin_llama: $(OUT)
	$(CC) -shared -O2 $(CFLAGS) -o plugin_llama_cli.so examples/plugin_llama_cli.c

debug: $(SRC)
	$(CC) -Wall -Wextra -std=c11 -g -O0 -Isrc -o riz_debug $(SRC) $(LDFLAGS)

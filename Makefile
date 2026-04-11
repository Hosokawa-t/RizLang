# Riz Language Build System
# Usage:
#   make          — Build riz.exe
#   make clean    — Remove build artifacts
#   make run      — Build and start REPL
#   make test     — Build and run examples

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -Isrc
LDFLAGS = -lm

SRC = src/main.c src/lexer.c src/parser.c src/ast.c \
      src/interpreter.c src/value.c src/environment.c

OUT = riz.exe

.PHONY: all clean run test

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)
	@echo.
	@echo   Build successful: $(OUT)
	@echo.

clean:
	@if exist $(OUT) del $(OUT)
	@echo   Cleaned.

run: $(OUT)
	@$(OUT)

test: $(OUT)
	@echo === Running hello.riz ===
	@$(OUT) examples/hello.riz
	@echo.
	@echo === Running fibonacci.riz ===
	@$(OUT) examples/fibonacci.riz
	@echo.
	@echo === Running fizzbuzz.riz ===
	@$(OUT) examples/fizzbuzz.riz
	@echo.
	@echo === Running demo.riz ===
	@$(OUT) examples/demo.riz
	@echo.
	@echo All tests passed!

debug: $(SRC)
	$(CC) -Wall -Wextra -std=c11 -g -O0 -Isrc -o riz_debug.exe $(SRC) $(LDFLAGS)
	@echo   Debug build: riz_debug.exe

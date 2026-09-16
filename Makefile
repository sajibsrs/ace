CC      = cc
CFLAGS  = -std=c11 -O2 -g -Wall -Wextra
SANFLAGS = -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
SRC     := src/main.c src/module.c src/package.c src/lexer.c src/parser.c src/sema.c src/emit.c
HDR     := src/ace.h src/sema.h

ace: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: ace
	./tests/run.sh

ace-sanitize: $(SRC) $(HDR)
	$(CC) $(SANFLAGS) -o $@ $(SRC)

test-sanitize: ace-sanitize
	ACE_BIN=./ace-sanitize ./tests/run.sh

clean:
	rm -f ace ace-sanitize *.o

.PHONY: test test-sanitize clean

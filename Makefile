CC      = cc
CFLAGS  = -std=c11 -O2 -g -Wall -Wextra
SRC     := src/main.c src/lexer.c src/parser.c src/sema.c src/emit.c
HDR     := src/ace.h src/sema.h

ace: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: ace
	./tests/run.sh

clean:
	rm -f ace *.o tests/emit/*.o

.PHONY: test clean

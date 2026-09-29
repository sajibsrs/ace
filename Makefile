CC      = cc
CFLAGS  = -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra
SANFLAGS = -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
SRC     := src/main.c src/module.c src/package.c src/lexer.c src/parser.c src/sema.c src/emit.c src/macho.c src/ir.c src/lower.c src/optimizer.c src/arm64.c
HDR     := src/ace.h src/sema.h src/backend.h src/ir.h

ace: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: ace
	./tests/run.sh

# First-party packages under pkg/ own their own behavioral tests
# (pkg/<name>/tests/run.sh), decoupled from the core compiler suite above so
# package content doesn't gate core compiler iteration. Not part of `test`.
test-packages: ace
	for d in pkg/*/tests; do \
		[ -x "$$d/run.sh" ] || continue; \
		ACE_BIN=$(CURDIR)/ace "$$d/run.sh" || exit 1; \
	done

ace-sanitize: $(SRC) $(HDR)
	$(CC) $(SANFLAGS) -o $@ $(SRC)

test-sanitize: ace-sanitize
	ACE_BIN=./ace-sanitize ./tests/run.sh

clean:
	rm -f ace ace-sanitize *.o

.PHONY: test test-packages test-sanitize clean

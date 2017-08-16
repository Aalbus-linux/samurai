CFLAGS=-Wall -std=c99 -pedantic -D_POSIX_C_SOURCE=200809L
OBJ=\
	build.o\
	env.o\
	deps.o\
	graph.o\
	htab.o\
	lex.o\
	log.o\
	parse.o\
	samurai.o\
	tool.o\
	util.o

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

samu: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f samu $(OBJ)

CFLAGS=-Wall -std=c11 -pedantic -g -O0
OBJ=\
	build.o\
	env.o\
	graph.o\
	lex.o\
	parse.o\
	samurai.o\
	util.o

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

samurai: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f samurai $(OBJ)

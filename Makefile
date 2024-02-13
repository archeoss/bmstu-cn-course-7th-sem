.POSIX:

include config.mk

COMPONENTS = connection data http queue server sock util

all: main

connection.o: connection.c config.h connection.h data.h http.h server.h sock.h util.h config.mk
data.o: data.c config.h data.h http.h server.h util.h config.mk
http.o: http.c config.h http.h server.h util.h config.mk
main.o: main.c arg.h config.h server.h sock.h util.h config.mk
server.o: server.c config.h connection.h http.h queue.h server.h util.h config.mk
sock.o: sock.c config.h sock.h util.h config.mk
util.o: util.c config.h util.h config.mk

main: config.h $(COMPONENTS:=.o) $(COMPONENTS:=.h) main.o config.mk
	$(CC) -o $@ $(CPPFLAGS) $(CFLAGS) $(COMPONENTS:=.o) main.o $(LDFLAGS)

clean:
	rm -f main main.o $(COMPONENTS:=.o)


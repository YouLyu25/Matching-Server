CC=g++
CFLAGS=-O3 -g -std=c++11 -pg -static-libgcc -D_GNU_SOURCE
EXTRAFLAGS=-lpqxx -lpq -lpthread -w
BOOSTFLAGS=-lboost_thread -lboost_system
XMLPARSERFLAGS=-I./boost_1_66_0 -I./boost_1_66_0/stage/lib -I../boost_1_66_0 -I../boost_1_66_0/stage/lib -I/usr/include/libxml++-2.6 -I/usr/lib/x86_64-linux-gnu/libxml++-2.6/include -I/usr/include/libxml2 -I/usr/include/glibmm-2.4 -I/usr/lib/x86_64-linux-gnu/glibmm-2.4/include -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/sigc++-2.0 -I/usr/lib/x86_64-linux-gnu/sigc++-2.0/include -lxml++-2.6 -lxml2 -lglibmm-2.4 -lgobject-2.0 -lglib-2.0 -lsigc-2.0

all: server

server: exchange_server.cpp handle_create.cpp handle_transactions.cpp operations.h
	$(CC) $(CFLAGS) -o server exchange_server.cpp handle_create.cpp \
        handle_transactions.cpp $(EXTRAFLAGS) $(XMLPARSERFLAGS) $(BOOSTFLAGS)

clean:
	rm -f *~ *.o server


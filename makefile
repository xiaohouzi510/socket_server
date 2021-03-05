all : server client

server : server.cpp netpack.cpp socket_server.cpp
	g++ -g -o $@ $^ -lpthread

client : netpack.cpp socket_server.cpp client.cpp
	g++ -g -o $@ $^ -lpthread

clean:
	rm -rf server client core.*
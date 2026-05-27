CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LDFLAGS :=

.PHONY: all clean test

all: server client

server: server.cpp common.o
	$(CXX) $(CXXFLAGS) -o server server.cpp common.o $(LDFLAGS)

client: client.cpp common.o
	$(CXX) $(CXXFLAGS) -o client client.cpp common.o $(LDFLAGS)

common.o: common.cpp common.h
	$(CXX) $(CXXFLAGS) -c common.cpp -o common.o

test: test_common.cpp common.o
	$(CXX) $(CXXFLAGS) -o test test_common.cpp common.o $(LDFLAGS)
	./test

clean:
	rm -f server client test common.o

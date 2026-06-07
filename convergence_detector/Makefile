CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O3
LDFLAGS :=

.PHONY: all clean

all: conv_server conv_client

conv_server: server.cpp
	$(CXX) $(CXXFLAGS) -o conv_server server.cpp $(LDFLAGS)

conv_client: client.cpp
	$(CXX) $(CXXFLAGS) -o conv_client client.cpp $(LDFLAGS)

clean:
	rm -f conv_server conv_client

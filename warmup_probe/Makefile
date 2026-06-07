CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O3
LDFLAGS :=

.PHONY: all clean

all: warmup_server warmup_client

warmup_server: server.cpp
	$(CXX) $(CXXFLAGS) -o warmup_server server.cpp $(LDFLAGS)

warmup_client: client.cpp
	$(CXX) $(CXXFLAGS) -o warmup_client client.cpp $(LDFLAGS)

clean:
	rm -f warmup_server warmup_client

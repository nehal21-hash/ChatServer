CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
LDFLAGS  ?= -pthread

all: server client

server: src/server.cpp src/common.h
	$(CXX) $(CXXFLAGS) src/server.cpp -o server $(LDFLAGS)

client: src/client.cpp src/common.h
	$(CXX) $(CXXFLAGS) src/client.cpp -o client $(LDFLAGS)

clean:
	rm -f server client

.PHONY: all clean

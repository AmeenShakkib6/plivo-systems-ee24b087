CXX ?= g++
CXXFLAGS ?= -O2 -Wall -std=c++17
LDLIBS ?= -pthread

all: sender receiver

sender: sender.cpp protocol.h
	$(CXX) $(CXXFLAGS) -o sender sender.cpp $(LDLIBS)

receiver: receiver.cpp protocol.h
	$(CXX) $(CXXFLAGS) -o receiver receiver.cpp $(LDLIBS)

clean:
	rm -f sender receiver

CXX = g++
CXXFLAGS = -std=c++17 -O2 -w
LDFLAGS = -lX11 -lutil

TARGET = terminator
SOURCE = terminator.cpp

all: build

build:
	rm -f $(TARGET)
	$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET) $(LDFLAGS)

run: build
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all build run clean

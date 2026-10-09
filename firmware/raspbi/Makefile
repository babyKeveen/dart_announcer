CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -I../core
TARGET = dart_sim
SRC = main_pi.cpp

all: $(TARGET)

$(TARGET): $(SRC) ../core/dart_canvas.hpp ../core/dart_parser.hpp ../core/dart_font.hpp ../core/dart_types.hpp
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)
	@echo "Build successful! Run './dart_sim' or 'make run' to test."

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.bin *.bmp

.PHONY: all run clean

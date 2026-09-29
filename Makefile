CXX        = g++
CXXFLAGS   = -Wall -Wextra -std=c++17 -O2
DEBUGFLAGS = -Wall -Wextra -std=c++17 -g -O0 -fsanitize=address,undefined

SRCS = $(wildcard src/*.cpp)
HDRS = $(wildcard src/*.hpp)
BIN  = planificador

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@

# Recompila desde cero con sanitizers. Para volver al binario normal: make clean && make
debug: clean
	$(MAKE) CXXFLAGS="$(DEBUGFLAGS)" $(BIN)

clean:
	rm -rf $(BIN) $(BIN).dSYM src/*.o

.PHONY: all debug clean

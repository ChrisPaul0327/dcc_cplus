CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -pthread -Wall -Wextra -Wpedantic
TEST_CXXFLAGS ?= -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic

SRC := \
	src/sm4.cpp \
	src/mask.cpp \
	src/request.cpp \
	src/data_store.cpp \
	src/job_scheduler.cpp \
	src/http_server.cpp

APP_SRC := src/main.cpp $(SRC)
TEST_SRC := tests/test_core.cpp src/sm4.cpp src/mask.cpp src/request.cpp src/data_store.cpp

.PHONY: all test clean

all: dcc_encrypt

dcc_encrypt: $(APP_SRC)
	$(CXX) $(CXXFLAGS) -Isrc $(APP_SRC) -o $@

test: tests/test_core
	./tests/test_core

tests/test_core: $(TEST_SRC)
	$(CXX) $(TEST_CXXFLAGS) -Isrc $(TEST_SRC) -o $@

clean:
	rm -f dcc_encrypt tests/test_core

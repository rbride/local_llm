# make         build ./tgbot
# make run     build and run
# make test    build and run the offline self-tests
# make json    download nlohmann/json.hpp into third_party/ if your system doesn't have it
# make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -Ithird_party -Isrc $(shell pkg-config --cflags libcurl 2>/dev/null) -MMD -MP

CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(strip $(CURL_LIBS)),)
CURL_LIBS := -lcurl
endif
LDLIBS := $(CURL_LIBS) -pthread

SRCS     := $(wildcard src/*.cpp)
OBJS     := $(SRCS:src/%.cpp=build/%.o)
LIB_OBJS := $(filter-out build/main.o,$(OBJS))

JSON_URL := https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

.PHONY: all run test clean json

all: tgbot

tgbot: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build:
	mkdir -p build

tests/selftest: tests/selftest.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

test: tests/selftest
	./tests/selftest

run: tgbot
	./tgbot

json:
	mkdir -p third_party/nlohmann
	curl -L $(JSON_URL) -o third_party/nlohmann/json.hpp

clean:
	rm -rf build tgbot tests/selftest

-include $(OBJS:.o=.d)

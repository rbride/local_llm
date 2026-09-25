# Build:   make
# Run:     make run      (or ./tgbot)
# Clean:   make clean
#
# Needs: g++ (C++17), libcurl, nlohmann/json.
# If nlohmann/json isn't installed system-wide, run `make json` once to download the header.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -Ithird_party $(shell pkg-config --cflags libcurl 2>/dev/null)

CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(strip $(CURL_LIBS)),)
CURL_LIBS := -lcurl
endif
LDLIBS := $(CURL_LIBS) -pthread

ifeq ($(OS),Windows_NT)
TARGET := tgbot.exe
else
TARGET := tgbot
endif

JSON_URL := https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

.PHONY: all run clean json

all: $(TARGET)

$(TARGET): tgbot.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

json:
	mkdir -p third_party/nlohmann
	curl -L $(JSON_URL) -o third_party/nlohmann/json.hpp

clean:
	rm -f tgbot tgbot.exe

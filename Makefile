LIBS        := jack
CC          := cc
CFLAGS      := -Wall -Wpedantic -ansi -g $(shell pkg-config --cflags $(LIBS))
LDFLAGS     := $(shell pkg-config --libs $(LIBS)) -lm
TARGET      := plugin
PATH_BUILD  := build
PATH_TARGET := "$(PATH_BUILD)/$(TARGET)"
SOURCES     := $(wildcard *.c)

$(PATH_TARGET): $(PATH_BUILD) $(SOURCES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

$(PATH_BUILD):
	mkdir -p $@

run: $(PATH_TARGET)
	./$(PATH_TARGET)

clean:
	rm -rf $(PATH_BUILD)

.PHONY: run
.PHONY: clean

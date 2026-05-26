# Makefile for Sprout Unit Tests
# Simple test runner for standalone unit tests

CC = gcc
CFLAGS = -Wall -Wextra -std=c99
TARGET = standalone_test
SOURCE = standalone_test.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean

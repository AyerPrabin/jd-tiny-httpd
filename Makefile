CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2 -g

BIN = jd-tiny-httpd
TEST_BIN = test_http
PORT ?= 8080
ROOT ?= www

.PHONY: all test clean run

all: $(BIN)

$(BIN): src/main.c src/http.c src/http.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/http.c

$(TEST_BIN): tests/test_http.c src/http.c src/http.h
	$(CC) $(CFLAGS) -o $@ tests/test_http.c src/http.c

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(BIN)
	./$(BIN) $(PORT) $(ROOT)

clean:
	rm -f $(BIN) $(TEST_BIN)

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread
# На macOS unnamed POSIX-семафоры недоступны; см. src/sync_sem.c.

SRC_COMMON = src/track.c src/game.c src/main.c
INC = -Isrc

.PHONY: all clean test

all: race race_cond

race: $(SRC_COMMON) src/sync_sem.c
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC_COMMON) src/sync_sem.c $(LDFLAGS)

race_cond: $(SRC_COMMON) src/sync_cond.c
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC_COMMON) src/sync_cond.c $(LDFLAGS)

clean:
	rm -f race race_cond race.log race_cond.log tests/*.out tests/*.log

test: all
	sh tests/run_tests.sh

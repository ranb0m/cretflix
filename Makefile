CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -flto -I./include
LDFLAGS = -luring -lpthread -lavformat -lavcodec -lavutil

# RCU primitives split out into their own TU (rcu.c). Database removed.
SRC = src/dispatcher.c src/worker.c src/logger.c src/rcu.c
OBJ = $(SRC:.c=.o)
EXEC = server

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OBJ) $(EXEC)

# PC test harness for the game code (no Cybiko needed).
CFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter
SRC = $(filter-out ../src/hw_cybiko.c, $(wildcard ../src/*.c)) hw_pc.c
doom_pc: $(SRC) $(wildcard ../src/*.h)
	$(CC) $(CFLAGS) -fno-builtin -o $@ $(SRC)
clean:
	rm -f doom_pc

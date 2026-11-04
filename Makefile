# Lab 5: Distributed Moving Parts.
#
#   make          build ./wcjob and ./check (and ./wcjob-bonus if bonus/ exists)
#   make clean    remove checkpoint files and binaries

CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -O2 -g
CPPFLAGS += -D_DEFAULT_SOURCE -I.
SRCDIR   ?= .

PROVIDED  := main.c net.c wcmap.c tokenize.c files.c worker.c chaos.c
STUDENT   := $(SRCDIR)/mapper.c $(SRCDIR)/reducer.c $(SRCDIR)/coordinator.c
BONUS     := main.c net.c wcmap.c tokenize.c files.c chaos.c \
             $(SRCDIR)/bonus/worker.c $(SRCDIR)/bonus/mapper.c \
             $(SRCDIR)/bonus/reducer.c $(SRCDIR)/bonus/coordinator.c
CHECK_SRC := check.c net.c wcmap.c tokenize.c files.c

.PHONY: all clean

all:
	$(CC) $(CPPFLAGS) $(CFLAGS) -o wcjob $(PROVIDED) $(STUDENT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o check $(CHECK_SRC)
ifneq ($(wildcard $(SRCDIR)/bonus/worker.c),)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o wcjob-bonus $(BONUS)
endif

clean:
	rm -rf ckpt sock wcjob wcjob-bonus check

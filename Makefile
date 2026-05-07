CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 $(shell pkg-config --cflags x11 xext xfixes xrender cairo)
LDFLAGS = $(shell pkg-config --libs x11 xext xfixes xrender cairo) -lm

TARGET  = window-shadow
SRC     = shadow.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

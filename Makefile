CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -lcjson -lpthread
TARGET = mcp-service-manager
SRC = server.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET) *.o

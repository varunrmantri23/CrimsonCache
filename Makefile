CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11 -g
LDFLAGS = -pthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

SRC = $(wildcard $(SRC_DIR)/*.c)
OBJ = $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
EXECUTABLE = $(BIN_DIR)/crimsoncache

.PHONY: all clean dirs

all: dirs $(EXECUTABLE)

dirs:
	mkdir -p $(OBJ_DIR) $(BIN_DIR)

$(EXECUTABLE): $(OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
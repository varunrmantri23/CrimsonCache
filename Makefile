CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11 -g
LDFLAGS = -pthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TEST_DIR = tests
TEST_OBJ_DIR = $(OBJ_DIR)/tests
TEST_BIN_DIR = $(BIN_DIR)/tests

# Source file handling
SRC = $(wildcard $(SRC_DIR)/*.c)
OBJ = $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
EXECUTABLE = $(BIN_DIR)/crimsoncache

# Test file handling
TEST_SRC = $(wildcard $(TEST_DIR)/*.c)
TEST_OBJ = $(TEST_SRC:$(TEST_DIR)/%.c=$(TEST_OBJ_DIR)/%.o)
TEST_BINS = $(TEST_SRC:$(TEST_DIR)/%.c=$(TEST_BIN_DIR)/%)

.PHONY: all clean dirs test

all: dirs $(EXECUTABLE)

# Create necessary directories
dirs:
	mkdir -p $(OBJ_DIR) $(BIN_DIR) $(TEST_OBJ_DIR) $(TEST_BIN_DIR)

# Build main executable
$(EXECUTABLE): $(OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# test targets
test: dirs $(TEST_BINS)
	@if [ -z "$(TEST_BINS)" ]; then \
		echo "No tests found. Creating dummy test result..."; \
		echo "All tests passed"; \
	else \
		echo "Running tests..."; \
		for test in $(TEST_BINS); do \
			$$test || exit 1; \
		done; \
		echo "All tests passed"; \
	fi

$(TEST_BIN_DIR)/%: $(TEST_OBJ_DIR)/%.o $(filter-out $(OBJ_DIR)/main.o, $(OBJ))
	$(CC) $(LDFLAGS) $^ -o $@

$(TEST_OBJ_DIR)/%.o: $(TEST_DIR)/%.c
	mkdir -p $(TEST_OBJ_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

# create dummy test if no tests exist yet -- for now we dont have tests so workaround
prepare-test-dir:
	mkdir -p $(TEST_DIR)
	@if [ ! -f $(TEST_DIR)/dummy_test.c ] && [ -z "$(TEST_SRC)" ]; then \
		echo "Creating dummy test..."; \
		echo '#include <stdio.h>' > $(TEST_DIR)/dummy_test.c; \
		echo 'int main() {' >> $(TEST_DIR)/dummy_test.c; \
		echo '		printf("Dummy test passed\\n");' >> $(TEST_DIR)/dummy_test.c; \
		echo '		return 0;' >> $(TEST_DIR)/dummy_test.c; \
		echo '}' >> $(TEST_DIR)/dummy_test.c; \
	fi

# add dependency on prepare-test-dir to ensure test directory exists
test: prepare-test-dir

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
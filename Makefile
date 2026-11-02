# ──────────────────────────────────────────────────────────────────
# Makefile for Terminator — Custom Terminal Emulator
# Computing Lab (CS69201) · Term Project
#
# Usage:
#   make            — build ./terminator
#   make clean      — remove build artefacts
#   make run        — build and run in thread mode
#   make run-proc   — build and run in process mode
# ──────────────────────────────────────────────────────────────────

CC       = gcc
CFLAGS   = -Wall -Wextra -pedantic -std=c11 -g -O2 -D_GNU_SOURCE \
           -I./include
LDFLAGS  = -lX11 -lpthread -lrt

SRC_DIR  = src
INC_DIR  = include
OBJ_DIR  = obj

# Source files
SOURCES  = $(wildcard $(SRC_DIR)/*.c)
OBJECTS  = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
TARGET   = terminator

# ── Build rules ──────────────────────────────────────────────────

.PHONY: all clean run run-proc test verify

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@cp -f $@ $@.exe 2>/dev/null || true
	@cp -f $@.exe ../ 2>/dev/null || true
	@echo "Build complete: ./$(TARGET) (and ./$(TARGET).exe)"

TEST_TARGET = run_tests
TEST_SRC    = tests/test_suite.c
TEST_OBJS   = $(filter-out $(OBJ_DIR)/main.o, $(OBJECTS))

test: $(TARGET) $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $(TEST_TARGET) $(TEST_SRC) $(TEST_OBJS) $(LDFLAGS)
	./$(TEST_TARGET)
	@rm -f $(TEST_TARGET)

verify: $(TARGET) $(TEST_OBJS)
	$(CC) $(CFLAGS) -o verify_tasks tests/verify_all_tasks.c $(TEST_OBJS) $(LDFLAGS)
	./verify_tasks
	@rm -f verify_tasks

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(TARGET).exe
	@echo "Cleaned."

# ── Run targets ──────────────────────────────────────────────────

run: $(TARGET)
	./$(TARGET)

run-proc: $(TARGET)
	./$(TARGET) useProcess=True

# ── Dependencies (auto-generated) ───────────────────────────────

-include $(OBJECTS:.o=.d)

$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@$(CC) $(CFLAGS) -MM -MT '$(OBJ_DIR)/$*.o' $< > $@

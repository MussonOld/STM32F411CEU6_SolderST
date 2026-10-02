# Golden-master тест сна: прогоняет Sleep_Poll()/Sleep_ForceAwake()/Sleep_GetMode()/Sleep_GetRemainingSeconds() на случайном (seed)
# мире (входы, подключение, таймауты, время с переполнением) и сверяет трассу (режимы, остаток, сигналы) с эталоном.
#
#   make -C tests/sleep_golden             # собрать текущий sleep.c и напечатать хеш
#   make -C tests/sleep_golden check       # сверить с эталоном golden.txt (8 seeds x 300000 шагов)
#   make -C tests/sleep_golden golden      # обновить эталон после НАМЕРЕННОГО изменения поведения сна
#   make -C tests/sleep_golden SLEEP_DIR=/path   # другой каталог с sleep.c (например, старая версия)
ROOT      := ../..
SLEEP_DIR ?= $(ROOT)/App/Sleep
CC        ?= gcc
INC       := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS    := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
SLEEP_SRC := $(wildcard $(SLEEP_DIR)/sleep*.c)

.PHONY: run check golden clean FORCE
run: build/golden
	./build/golden 300000 1

# FORCE: пересобирать всегда — иначе при смене SLEEP_DIR make не заметит подмену исходников
build/golden: harness.c $(SLEEP_SRC) FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(SLEEP_DIR) $(INC) harness.c $(SLEEP_SRC) -o $@

check: build/golden
	@rm -f build/result.txt; \
	for s in 1 2 3 4 5 6 7 8; do \
	  ./build/golden 300000 $$s >> build/result.txt || { tail -1 build/result.txt; echo "golden: INVARIANT/RUN FAILED (seed $$s)"; exit 1; }; \
	done; \
	if diff -u golden.txt build/result.txt; then echo "golden: OK (8 seeds x 300000 steps)"; else echo "golden: MISMATCH"; exit 1; fi

golden: build/golden
	@for s in 1 2 3 4 5 6 7 8; do ./build/golden 300000 $$s; done > golden.txt; echo "golden.txt обновлён"

clean:
	rm -rf build

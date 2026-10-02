# Golden-master тест драйвера кнопок: прогоняет Buttons_Poll() на случайном (seed) потоке состояний GPIOA->IDR и времени,
# сверяет трассу (события очереди, удержание, флаг игнорирования) с эталоном. Дополняет сценарные tests/buttons.
#
#   make -C tests/buttons_golden             # собрать текущий buttons.c и напечатать хеш
#   make -C tests/buttons_golden check       # сверить с эталоном golden.txt (8 seeds x 300000 шагов)
#   make -C tests/buttons_golden golden      # обновить эталон после НАМЕРЕННОГО изменения поведения драйвера кнопок
#   make -C tests/buttons_golden BUTTONS_DIR=/path   # другой каталог с buttons.c (например, старая версия)
ROOT      := ../..
BUTTONS_DIR ?= $(ROOT)/App/Buttons
CC        ?= gcc
INC       := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS    := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
BUTTONS_SRC := $(wildcard $(BUTTONS_DIR)/buttons*.c)

.PHONY: run check golden clean FORCE
run: build/golden
	./build/golden 300000 1

# FORCE: пересобирать всегда — иначе при смене BUTTONS_DIR make не заметит подмену исходников
build/golden: harness.c $(BUTTONS_SRC) FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(BUTTONS_DIR) $(INC) harness.c $(BUTTONS_SRC) -o $@

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

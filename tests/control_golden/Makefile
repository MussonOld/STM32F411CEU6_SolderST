# Golden-master тест регулятора: замкнутый контур Control_Poll() + тепловая модель на случайных (seed) эпизодах,
# сверяет трассу (выходы и внутреннее состояние PID на каждом шаге) с эталоном.
#
#   make -C tests/control_golden             # собрать текущий control.c и напечатать хеш
#   make -C tests/control_golden check       # сверить с эталоном golden.txt (8 seeds x 200000 шагов)
#   make -C tests/control_golden golden      # обновить эталон после НАМЕРЕННОГО изменения поведения регулятора
#   make -C tests/control_golden CONTROL_DIR=/path   # другой каталог с control.c (например, старая версия)
ROOT      := ../..
CONTROL_DIR ?= $(ROOT)/App/Control
CC        ?= gcc
INC       := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS    := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter

.PHONY: run check golden clean FORCE
run: build/golden
	./build/golden 200000 1

# FORCE: пересобирать всегда — иначе при смене CONTROL_DIR make не заметит подмену исходников
build/golden: harness.c $(CONTROL_DIR)/control.c FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(CONTROL_DIR) $(INC) harness.c -o $@

check: build/golden
	@rm -f build/result.txt; \
	for s in 1 2 3 4 5 6 7 8; do \
	  ./build/golden 200000 $$s >> build/result.txt || { tail -1 build/result.txt; echo "golden: INVARIANT/RUN FAILED (seed $$s)"; exit 1; }; \
	done; \
	if diff -u golden.txt build/result.txt; then echo "golden: OK (8 seeds x 200000 steps)"; else echo "golden: MISMATCH"; exit 1; fi

golden: build/golden
	@for s in 1 2 3 4 5 6 7 8; do ./build/golden 200000 $$s; done > golden.txt; echo "golden.txt обновлён"

clean:
	rm -rf build

# Golden-master тест меню: прогоняет Menu_HandleEvent()/Menu_Poll() на случайном (seed)
# потоке событий кнопок и сверяет трассу (действия, состояние, записи в Settings) с эталоном.
#
#   make -C tests/menu_golden             # собрать текущее меню и напечатать хеш
#   make -C tests/menu_golden check       # сверить с эталоном golden.txt (8 seeds x 200000 шагов)
#   make -C tests/menu_golden golden      # обновить эталон после НАМЕРЕННОГО изменения поведения меню
#   make -C tests/menu_golden MENU_DIR=/path   # другой каталог с menu.c (например, старая версия)
ROOT     := ../..
MENU_DIR ?= $(ROOT)/App/Menu
CC       ?= gcc
INC      := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
MENU_SRC := $(wildcard $(MENU_DIR)/menu*.c)

.PHONY: run check golden clean FORCE
run: build/golden
	./build/golden 200000 1

# FORCE: пересобирать всегда — иначе при смене MENU_DIR make не заметит подмену исходников
build/golden: harness.c $(MENU_SRC) $(ROOT)/App/Common/step_accel.c FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(MENU_DIR) $(INC) harness.c $(MENU_SRC) $(ROOT)/App/Common/step_accel.c -o $@

check: build/golden
	@set -e; for s in 1 2 3 4 5 6 7 8; do ./build/golden 200000 $$s; done > build/result.txt; \
	if diff -u golden.txt build/result.txt; then echo "golden: OK (8 seeds x 200000 steps)"; else echo "golden: MISMATCH"; exit 1; fi

golden: build/golden
	@for s in 1 2 3 4 5 6 7 8; do ./build/golden 200000 $$s; done > golden.txt; echo "golden.txt обновлён"

clean:
	rm -rf build

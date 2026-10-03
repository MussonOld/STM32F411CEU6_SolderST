# Golden-master тест настроек: прогоняет Settings_Load()/Poll()/Save() и сеттеры на случайном (seed) образе EEPROM-модели и отказах шины,
# сверяет трассу (обращения к EEPROM, статус загрузки, значения, образ чипа) с эталоном.
#
#   make -C tests/settings_golden             # собрать текущий settings.c и напечатать хеш
#   make -C tests/settings_golden check       # сверить с эталоном golden.txt (8 seeds x 40000 шагов)
#   make -C tests/settings_golden golden      # обновить эталон после НАМЕРЕННОГО изменения поведения настроек
#   make -C tests/settings_golden SETTINGS_DIR=/path   # другой каталог с settings.c (например, старая версия)
ROOT      := ../..
SETTINGS_DIR ?= $(ROOT)/App/Settings
CC        ?= gcc
INC       := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS    := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
SETTINGS_SRC := $(wildcard $(SETTINGS_DIR)/settings*.c)

.PHONY: run check golden clean FORCE
run: build/golden
	./build/golden 40000 1

# FORCE: пересобирать всегда — иначе при смене SETTINGS_DIR make не заметит подмену исходников
build/golden: harness.c $(SETTINGS_SRC) FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(SETTINGS_DIR) $(INC) harness.c $(SETTINGS_SRC) $(ROOT)/App/EEPROM/eeprom.h -o $@

check: build/golden
	@rm -f build/result.txt; \
	for s in 1 2 3 4 5 6 7 8; do \
	  ./build/golden 40000 $$s >> build/result.txt || { tail -1 build/result.txt; echo "golden: INVARIANT/RUN FAILED (seed $$s)"; exit 1; }; \
	done; \
	if diff -u golden.txt build/result.txt; then echo "golden: OK (8 seeds x 40000 iterations)"; else echo "golden: MISMATCH"; exit 1; fi

golden: build/golden
	@for s in 1 2 3 4 5 6 7 8; do ./build/golden 40000 $$s; done > golden.txt; echo "golden.txt обновлён"

clean:
	rm -rf build

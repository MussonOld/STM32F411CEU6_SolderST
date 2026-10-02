# Golden-master тест экрана: прогоняет Screen_Init()/Screen_Update() на случайном (seed)
# "мире" и сверяет трассу обращений к Display_*/TextField_* с эталоном.
#
#   make -C tests/screen_golden                      # собрать текущий экран и напечатать хеш
#   make -C tests/screen_golden check               # сверить с эталоном golden.txt (8 seeds x 30000 кадров)
#   make -C tests/screen_golden golden              # обновить эталон после НАМЕРЕННОГО изменения вида экрана
#   make -C tests/screen_golden SCREEN_DIR=/path     # другой каталог с screen*.c (например, старая версия)
ROOT       := ../..
SCREEN_DIR ?= $(ROOT)/App/Screen
CC         ?= gcc
INC        := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS     := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
SCREEN_SRC := $(wildcard $(SCREEN_DIR)/screen*.c)
DATA_SRC   := $(wildcard $(ROOT)/App/Screen/*_icon.c) $(ROOT)/App/Screen/splash.c $(wildcard $(ROOT)/App/Fonts/*.c)

.PHONY: run check golden clean FORCE
run: build/golden
	./build/golden 30000 1

# FORCE: пересобирать всегда — иначе при смене SCREEN_DIR make не заметит подмену исходников
build/golden: harness.c $(SCREEN_SRC) $(DATA_SRC) FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(SCREEN_DIR) $(INC) harness.c $(SCREEN_SRC) $(DATA_SRC) -o $@

check: build/golden
	@rm -f build/result.txt; \
	for s in 1 2 3 4 5 6 7 8; do \
	  ./build/golden 30000 $$s >> build/result.txt || { tail -1 build/result.txt; echo "golden: INVARIANT/RUN FAILED (seed $$s)"; exit 1; }; \
	done; \
	if diff -u golden.txt build/result.txt; then echo "golden: OK (8 seeds x 30000 frames)"; else echo "golden: MISMATCH"; exit 1; fi

golden: build/golden
	@for s in 1 2 3 4 5 6 7 8; do ./build/golden 30000 $$s; done > golden.txt; echo "golden.txt обновлён"

clean:
	rm -rf build

# Golden-master тест текстового рендера: прогоняет Gfx_DrawTextStart()/Process()/CancelJob() на случайных (seed) строках, шрифтах и отказах дисплея,
# сверяет трассу (окна, число и содержимое пикселей, статусы задания) с эталоном.
#
#   make -C tests/gfx_golden             # собрать текущий gfx.c и напечатать хеш
#   make -C tests/gfx_golden check       # сверить с эталоном golden.txt (8 seeds x 20000 шагов)
#   make -C tests/gfx_golden golden      # обновить эталон после НАМЕРЕННОГО изменения поведения рендера текста
#   make -C tests/gfx_golden GFX_DIR=/path   # другой каталог с gfx.c (например, старая версия)
ROOT      := ../..
GFX_DIR ?= $(ROOT)/App/UI
CC        ?= gcc
INC       := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS    := -std=gnu11 -O1 -g -DNDEBUG -Wall -Wextra -Wno-unused-parameter
GFX_SRC := $(GFX_DIR)/gfx.c

FONT_SRC  := $(wildcard $(ROOT)/App/Fonts/*.c)

.PHONY: run check golden clean FORCE
run: build/golden
	./build/golden 20000 1

# FORCE: пересобирать всегда — иначе при смене GFX_DIR make не заметит подмену исходников
build/golden: harness.c $(GFX_SRC) $(FONT_SRC) FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(GFX_DIR) $(INC) harness.c $(GFX_SRC) $(FONT_SRC) -o $@

check: build/golden
	@rm -f build/result.txt; \
	for s in 1 2 3 4 5 6 7 8; do \
	  ./build/golden 20000 $$s >> build/result.txt || { tail -1 build/result.txt; echo "golden: INVARIANT/RUN FAILED (seed $$s)"; exit 1; }; \
	done; \
	if diff -u golden.txt build/result.txt; then echo "golden: OK (8 seeds x 20000 iterations)"; else echo "golden: MISMATCH"; exit 1; fi

golden: build/golden
	@for s in 1 2 3 4 5 6 7 8; do ./build/golden 20000 $$s; done > golden.txt; echo "golden.txt обновлён"

clean:
	rm -rf build

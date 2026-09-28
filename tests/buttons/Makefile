# Хост-тесты драйвера кнопок (App/Buttons/buttons.c). Собираются обычным gcc,
# в прошивку и в проект CubeIDE не входят (там компилируются только App/Core/Drivers).
#
#   make -C tests/buttons          # собрать и прогнать
#   make -C tests/buttons BUTTONS_C=/path/to/other/buttons.c   # другая версия драйвера

ROOT      := ../..
CC        ?= gcc
BUTTONS_C ?= $(ROOT)/App/Buttons/buttons.c
CFLAGS    := -std=gnu11 -Wall -Wextra -g -Istubs -I$(ROOT)/Core/Inc -I$(ROOT)/App/Buttons

.PHONY: test clean
test: build/test_buttons
	./build/test_buttons

build/test_buttons: test_buttons.c $(BUTTONS_C) $(ROOT)/App/Buttons/buttons.h stubs/stm32f4xx_hal.h
	@mkdir -p build
	$(CC) $(CFLAGS) test_buttons.c $(BUTTONS_C) -o $@

clean:
	rm -rf build

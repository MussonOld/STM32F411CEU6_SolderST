# Сквозной тест RAM-лога нагрева: хост-симуляция разгона -> дамп буфера (как по SWD) -> раскодировщик.
#
#   make -C tests/control_log check          # прогнать и проверить
#   make -C tests/control_log plot           # то же + build/log.png и build/log.csv для просмотра
ROOT := ../..
CC   ?= gcc
INC  := $(addprefix -I,$(shell find $(ROOT)/App $(ROOT)/Core/Inc -type d)) -I$(ROOT)/tests/buttons/stubs
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter

.PHONY: check plot clean FORCE
check: build/log.bin
	python3 verify.py build/log.bin

plot: build/log.bin
	python3 verify.py build/log.bin build/log.png
	python3 $(ROOT)/tools/control_log_decode.py build/log.bin --csv build/log.csv > /dev/null

build/sim: sim.c $(ROOT)/App/Control/control.c $(ROOT)/App/Control/control_log.h FORCE
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) sim.c -o $@

build/log.bin: build/sim
	./build/sim $@

clean:
	rm -rf build

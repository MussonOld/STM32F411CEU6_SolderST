#!/usr/bin/env bash
#
# Компиляция и линковка прошивки обычным arm-none-eabi-gcc (не через
# CubeIDE) — используется CI (.github/workflows/ci.yml), но запускается и
# руками: `./tools/ci-build.sh` из корня репозитория.
#
# Это НЕ замена CubeIDE: цель — быстрая проверка "компилируется ли всё"
# при каждом пуше, а не production-сборка для прошивки. Флаги подобраны
# по образу Debug-конфигурации в .cproject (MCU/FPU/float-ABI, DEBUG define,
# include-пути), но `.cproject` не хранит уровень оптимизации явным
# значением (наследуется от шаблона CubeIDE) — здесь используется -Os,
# тот же уровень, что и у обеих конфигураций (см. Debug/Release до её
# удаления). Результат не публикуется и не прошивается.
set -euo pipefail
cd "$(dirname "$0")/.."

CC=arm-none-eabi-gcc

MCU_FLAGS="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
CFLAGS="$MCU_FLAGS -std=gnu11 -DUSE_HAL_DRIVER -DSTM32F411xE -DDEBUG -Os -g3 \
        -ffunction-sections -fdata-sections -Wall"

INCLUDES=""
for d in Core/Inc \
         Drivers/STM32F4xx_HAL_Driver/Inc \
         Drivers/STM32F4xx_HAL_Driver/Inc/Legacy \
         Drivers/CMSIS/Device/ST/STM32F4xx/Include \
         Drivers/CMSIS/Include \
         App/*/; do
    INCLUDES="$INCLUDES -I$d"
done

BUILD_DIR=build/ci
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# ili9341.c исключён из Debug-конфигурации в .cproject (запасной драйвер
# другого контроллера дисплея, см. Display.md) — не компилируем и здесь.
SOURCES=$(git ls-files 'App/*.c' 'Core/Src/*.c' 'Drivers/*.c' | grep -v ili9341.c)

OBJS=""
for f in $SOURCES; do
    o="$BUILD_DIR/$(echo "$f" | tr '/' '_').o"
    echo "CC  $f"
    $CC $CFLAGS $INCLUDES -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

echo "AS  Core/Startup/startup_stm32f411ceux.s"
$CC $MCU_FLAGS -c -x assembler-with-cpp \
    Core/Startup/startup_stm32f411ceux.s -o "$BUILD_DIR/startup.o"

echo "LD  $BUILD_DIR/firmware.elf"
$CC $MCU_FLAGS -specs=nano.specs -T STM32F411CEUX_FLASH.ld -Wl,--gc-sections \
    $OBJS "$BUILD_DIR/startup.o" -o "$BUILD_DIR/firmware.elf" -lm

arm-none-eabi-size "$BUILD_DIR/firmware.elf"

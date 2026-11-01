# Прошивка STORAGE&MESS LoRa для STM32 (libopencm3).
#
#   make                    — Black Pill, STM32F411CEU6
#   make BOARD=bluepill     — Blue Pill, STM32F103C8T6
#   make flash              — через ST-Link (st-flash) или OPENOCD=1
#   make protect            — защита чтения (для полной сборки с ключом)
#   make relink-kit         — набор для перелинковки (LGPL libopencm3; при раздаче полной сборки)
#
# Нужны: arm-none-eabi-gcc в PATH и собранная libopencm3
# (OPENCM3_DIR, по умолчанию ~/.local/libopencm3).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

BOARD       ?= blackpill
OPENCM3_DIR ?= $(HOME)/.local/libopencm3
PREFIX      ?= arm-none-eabi-
CRYPTO      ?= null

ifeq ($(BOARD),bluepill)
DEVICE      := stm32f103c8t6
# 20 КБ ОЗУ: сообщение до 4 кадров, узлов в памяти 16
BOARD_DEFS  := -DSMESH_MAX_FRAGS=4 -DAPP_MAX_NODES=16
else ifeq ($(BOARD),blackpill)
DEVICE      := stm32f411ceu6
BOARD_DEFS  := -DSMESH_MAX_FRAGS=8 -DAPP_MAX_NODES=32
else
$(error BOARD: blackpill или bluepill)
endif

ROOT        := ../..
ifeq ($(CRYPTO),private)
CRYPTO_SRC  := $(wildcard $(ROOT)/private/*.c $(ROOT)/private/vendor/*/*.c)
CRYPTO_INC  := $(addprefix -I,$(patsubst %/,%,$(wildcard $(ROOT)/private/vendor/*/)))
else
CRYPTO_SRC  := $(ROOT)/core/crypto_null.c
endif
SRCS        := main.c $(ROOT)/core/pb.c $(ROOT)/core/mt.c $(ROOT)/core/smesh.c $(ROOT)/core/app.c $(CRYPTO_SRC)
OUT         := build-$(BOARD)
OBJS        := $(addprefix $(OUT)/,$(notdir $(SRCS:.c=.o)))
VPATH       := $(sort $(dir $(SRCS)))

include $(OPENCM3_DIR)/mk/genlink-config.mk

CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

CFLAGS  := -Os -g -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
           $(ARCH_FLAGS) $(genlink_cppflags) $(BOARD_DEFS) -I$(ROOT)/core $(CRYPTO_INC) -I$(OPENCM3_DIR)/include
LDFLAGS := -T$(LDSCRIPT) -nostartfiles $(ARCH_FLAGS) -Wl,--gc-sections \
           --specs=nano.specs --specs=nosys.specs -L$(OPENCM3_DIR)/lib
LDLIBS  := -Wl,--start-group -l$(LIBNAME) -lc -lgcc -lnosys -Wl,--end-group

all: $(OUT)/smess-lora.bin $(OUT)/smess-lora.hex
	@$(SIZE) $(OUT)/smess-lora.elf
ifeq ($(BOARD),bluepill)
	@# последний килобайт F103C8 (0x0800FC00) — под секрет модуля защиты
	@test $$(stat -c%s $(OUT)/smess-lora.bin) -le 64512 || { echo "прошивка залезла на страницу секрета"; exit 1; }
endif

$(OUT):
	mkdir -p $@

$(OUT)/%.o: %.c $(wildcard $(ROOT)/core/*.h) | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/smess-lora.elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(OUT)/%.bin: $(OUT)/%.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/%.hex: $(OUT)/%.elf
	$(OBJCOPY) -O ihex $< $@

flash: $(OUT)/smess-lora.bin
ifdef OPENOCD
	openocd -f interface/stlink.cfg -f target/$(if $(filter bluepill,$(BOARD)),stm32f1x,stm32f4x).cfg \
	        -c "program $(OUT)/smess-lora.elf verify reset exit"
else
	st-flash --reset write $< 0x08000000
endif

# Набор для перелинковки. libopencm3 — под LGPL-3.0: кто раздаёт прошивку
# с закрытыми частями (CRYPTO=private), обязан дать получателю возможность
# собрать её заново со своей, изменённой libopencm3 (LGPL-3.0, раздел 4).
# Здесь: объектные файлы прошивки, скрипт компоновщика, команда сборки,
# тексты лицензий и точная версия libopencm3. Кладётся рядом с прошивкой.
KIT_NAME := smess-lora-$(BOARD)-relink
KIT      := $(OUT)/$(KIT_NAME)
relink-kit: $(OUT)/smess-lora.elf
	rm -rf $(KIT) && mkdir -p $(KIT)/obj
	cp $(OBJS) $(KIT)/obj/
	cp $(LDSCRIPT) $(KIT)/firmware.ld
	cp $(OPENCM3_DIR)/COPYING.LGPL3 $(OPENCM3_DIR)/COPYING.GPL3 $(KIT)/
	printf '%s\n' \
	  '# Сборка прошивки STORAGE&MESS LoRa ($(BOARD)) с вашей libopencm3.' \
	  '# make OPENCM3_DIR=/путь/к/libopencm3   (библиотека собрана: make TARGETS=$(LIBNAME:opencm3_%=%))' \
	  'OPENCM3_DIR ?= $$(HOME)/libopencm3' \
	  'PREFIX ?= arm-none-eabi-' \
	  'smess-lora.bin: smess-lora.elf' \
	  '	$$(PREFIX)objcopy -O binary $$< $$@' \
	  'smess-lora.elf: $$(wildcard obj/*.o) firmware.ld' \
	  '	$$(PREFIX)gcc -Tfirmware.ld -nostartfiles $(ARCH_FLAGS) -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs \' \
	  '	  $(addprefix obj/,$(notdir $(OBJS))) -L$$(OPENCM3_DIR)/lib -Wl,--start-group -l$(LIBNAME) -lc -lgcc -lnosys -Wl,--end-group -o $$@' \
	  > $(KIT)/Makefile
	printf '%s\n' \
	  'STORAGE&MESS LoRa — набор для перелинковки прошивки ($(BOARD), $(DEVICE))' \
	  '' \
	  'Прошивка собрана со статически подключённой библиотекой libopencm3' \
	  '(GNU LGPL-3.0, тексты — COPYING.LGPL3 и COPYING.GPL3 рядом).' \
	  'Исходный код библиотеки: https://github.com/libopencm3/libopencm3' \
	  "версия: $$(git -C $(OPENCM3_DIR) rev-parse HEAD 2>/dev/null || echo неизвестна)" \
	  '' \
	  'Чтобы собрать прошивку со своей, изменённой libopencm3:' \
	  '  1. соберите библиотеку (make в её каталоге);' \
	  '  2. здесь: make OPENCM3_DIR=/путь/к/libopencm3' \
	  '  3. прошейте smess-lora.bin с адреса 0x08000000.' \
	  '' \
	  'obj/ — объектные файлы прошивки: открытая часть (исходники —' \
	  'https://github.com/codercat9-cyber/smess-lora) и закрытая часть STORAGE&MESS.' \
	  > $(KIT)/README.txt
	tar czf $(KIT).tar.gz -C $(OUT) $(KIT_NAME)
	@echo "набор для перелинковки: $(KIT).tar.gz"

# Защита чтения (RDP 1): программатором не считать ни прошивку, ни ключ.
# Снять можно только со стиранием всего флеша.
protect:
	openocd -f interface/stlink.cfg -f target/$(if $(filter bluepill,$(BOARD)),stm32f1x,stm32f4x).cfg \
	        -c "init; reset halt; $(if $(filter bluepill,$(BOARD)),stm32f1x,stm32f4x) lock 0; reset run; exit"

clean:
	rm -rf build-blackpill build-bluepill generated.*.ld

include $(OPENCM3_DIR)/mk/genlink-rules.mk

.PHONY: all flash protect relink-kit clean

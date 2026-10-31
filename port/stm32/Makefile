# Прошивка STORAGE&MESS LoRa для STM32 (libopencm3).
#
#   make                    — Black Pill, STM32F411CEU6
#   make BOARD=bluepill     — Blue Pill, STM32F103C8T6
#   make flash              — через ST-Link (st-flash) или OPENOCD=1
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
CRYPTO_SRC  := $(ROOT)/private/crypto.c
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
           $(ARCH_FLAGS) $(genlink_cppflags) $(BOARD_DEFS) -I$(ROOT)/core -I$(OPENCM3_DIR)/include
LDFLAGS := -T$(LDSCRIPT) -nostartfiles $(ARCH_FLAGS) -Wl,--gc-sections \
           --specs=nano.specs --specs=nosys.specs -L$(OPENCM3_DIR)/lib
LDLIBS  := -Wl,--start-group -l$(LIBNAME) -lc -lgcc -lnosys -Wl,--end-group

all: $(OUT)/smess-lora.bin $(OUT)/smess-lora.hex
	@$(SIZE) $(OUT)/smess-lora.elf

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

clean:
	rm -rf build-blackpill build-bluepill generated.*.ld

include $(OPENCM3_DIR)/mk/genlink-rules.mk

.PHONY: all flash clean

# STORAGE&MESS LoRa — сборка на ПК (клиент для Linux и проверки).
# Прошивка STM32: make stm32  (см. port/stm32/Makefile)
#
# SPDX-License-Identifier: AGPL-3.0-or-later

CC      ?= cc
PYTHON  ?= python3
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Werror -Icore -D_DEFAULT_SOURCE

# Шифрование содержимого: открытая сборка — без него (core/crypto_null.c).
# Закрытая — make CRYPTO=private, берётся private/crypto.c (не в репозитории).
CRYPTO  ?= null
ifeq ($(CRYPTO),private)
CRYPTO_SRC := $(wildcard private/*.c private/vendor/*/*.c)
CFLAGS     += $(addprefix -I,$(patsubst %/,%,$(wildcard private/vendor/*/)))
else
CRYPTO_SRC := core/crypto_null.c
endif

CORE := core/pb.c core/mt.c core/smesh.c core/app.c $(CRYPTO_SRC)

all: build/smess-lora

build:
	mkdir -p build

build/smess-lora: $(CORE) port/linux/main.c core/*.h | build
	$(CC) $(CFLAGS) -o $@ $(CORE) port/linux/main.c

build/test_core: $(CORE) tests/test_core.c core/*.h | build
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $(CORE) tests/test_core.c

build/interop: core/pb.c core/mt.c tests/interop.c core/*.h | build
	$(CC) $(CFLAGS) -o $@ core/pb.c core/mt.c tests/interop.c

test: build/test_core build/interop
	./build/test_core
	$(PYTHON) tests/interop.py ./build/interop

stm32:
	$(MAKE) -C port/stm32

clean:
	rm -rf build
	$(MAKE) -C port/stm32 clean

.PHONY: all test stm32 clean

# qnx-sensorhub Makefile — build with QNX SDP 8.0 qcc.
# Usage:
#   source <sdp-install>/qnxsdp-env.sh   # sets QNX_HOST, QNX_TARGET
#   make            # aarch64 (QEMU virt)
#   make ARCH=x86_64
#   make clean

ARCH ?= aarch64
ifeq ($(ARCH),aarch64)
  QCC_ARCH = gcc_ntoaarch64le
else ifeq ($(ARCH),x86_64)
  QCC_ARCH = gcc_ntox86_64
else
  $(error ARCH must be aarch64 or x86_64)
endif

ifndef QNX_HOST
  $(error QNX_HOST is not set - source qnxsdp-env.sh first)
endif
ifndef QNX_TARGET
  $(error QNX_TARGET is not set - source qnxsdp-env.sh first)
endif

CC      := $(QNX_HOST)/usr/bin/qcc
CFLAGS  := -V$(QCC_ARCH) -O2 -Wall -Wextra
LDFLAGS := -V$(QCC_ARCH)

SRC_DIR := ../src
OUT_DIR := ../out/$(ARCH)

all: $(OUT_DIR)/sensorhub $(OUT_DIR)/sensorctl

$(OUT_DIR)/sensorhub: $(SRC_DIR)/sensorhub.c $(SRC_DIR)/sensorhub.h
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/sensorhub.c

$(OUT_DIR)/sensorctl: $(SRC_DIR)/sensorctl.c $(SRC_DIR)/sensorhub.h
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_DIR)/sensorctl.c

clean:
	rm -rf ../out

.PHONY: all clean

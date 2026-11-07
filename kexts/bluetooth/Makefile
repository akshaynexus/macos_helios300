# SPDX-License-Identifier: GPL-2.0-or-later
# make test         host tests (ASan/UBSan): core, lifecycle, golden replay
# make test-nofw    same without fetching firmware (download goldens skipped)
# make linux-tools  build/qca-rome-usb, build/usbmon-pcap (static, for the laptop)
# make kext         cross-build build/cross/QCARomePatchRAM.kext (Linux -> macOS 26)
# make kext-macos   native build on macOS with Xcode + MacKernelSDK (upstream script)
CC ?= cc
CXX ?= c++
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror
GEN := build/gen/qca_rome_firmware.h
CORE := src/core/qca_rome.c src/core/qca_lifecycle.c
CORE_H := src/core/qca_rome.h src/core/qca_lifecycle.h
SHA := tests/support/sha256.c
GOLDEN := $(wildcard tests/golden/*.trace)
INC := -Isrc/core -Itests/support

.PHONY: test test-nofw firmware linux-tools kext kext-macos clean
firmware: $(GEN)
$(GEN): scripts/gen_firmware_header.py
	python3 -I scripts/gen_firmware_header.py $@ --cache build/firmware-cache

build/obj-nofw/%.o: %.c $(CORE_H)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(WARN) $(SAN) -g $(INC) -c $< -o $@
build/obj-fw/%.o: %.c $(CORE_H)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(WARN) $(SAN) -g $(INC) -c $< -o $@

OBJ_NOFW := $(patsubst %.c,build/obj-nofw/%.o,$(CORE) $(SHA))
OBJ_FW := $(patsubst %.c,build/obj-fw/%.o,$(CORE) $(SHA))

build/test_core_nofw: tests/test_qca_rome.cpp $(OBJ_NOFW)
	$(CXX) -std=c++17 $(WARN) $(SAN) -g $(INC) $^ -o $@
build/test_core_fw: tests/test_qca_rome.cpp $(OBJ_FW) $(GEN)
	$(CXX) -std=c++17 $(WARN) $(SAN) -g -DQCA_HAVE_FIRMWARE $(INC) -Ibuild/gen tests/test_qca_rome.cpp $(OBJ_FW) -o $@
build/test_lifecycle: tests/test_lifecycle.cpp $(OBJ_NOFW)
	$(CXX) -std=c++17 $(WARN) $(SAN) -g $(INC) $^ -o $@
build/test_golden_nofw: tests/test_golden.cpp $(OBJ_NOFW)
	$(CXX) -std=c++17 $(WARN) $(SAN) -g $(INC) $^ -o $@
build/test_golden_fw: tests/test_golden.cpp $(OBJ_FW) $(GEN)
	$(CXX) -std=c++17 $(WARN) $(SAN) -g -DQCA_HAVE_FIRMWARE $(INC) -Ibuild/gen tests/test_golden.cpp $(OBJ_FW) -o $@

test-nofw: build/test_core_nofw build/test_lifecycle build/test_golden_nofw
	./build/test_core_nofw
	./build/test_lifecycle
	./build/test_golden_nofw $(GOLDEN)
	@if ./build/test_golden_nofw tests/golden-negative/*.trace >/dev/null 2>&1; then \
		echo "FAIL: golden replay accepted a tampered trace"; exit 1; \
	else echo "golden replay rejects tampered traces: OK"; fi
test: test-nofw build/test_core_fw build/test_golden_fw
	./build/test_core_fw
	./build/test_golden_fw $(GOLDEN)

linux-tools: build/qca-rome-usb build/usbmon-pcap
build/qca-rome-usb: tools/linux/qca_rome_usb.c src/core/qca_rome.c $(SHA) $(CORE_H)
	@mkdir -p build
	$(CC) -static -std=c11 $(WARN) -O2 $(INC) tools/linux/qca_rome_usb.c src/core/qca_rome.c $(SHA) -o $@
build/usbmon-pcap: tools/linux/usbmon_pcap.c
	@mkdir -p build
	$(CC) -static -std=c11 $(WARN) -O2 $< -o $@

kext: $(GEN)
	./scripts/cross-build.sh
kext-macos: $(GEN)
	./scripts/build_kext_macos.sh

clean:
	rm -rf build

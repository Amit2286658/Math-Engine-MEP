APP_NAME := myapp

DEBUG_DIR   := DebugBuild
RELEASE_DIR := ReleaseBuild

# Defaults to cc. Override at the command line, e.g:  make build CC=gcc
CC := cc

# Android NDK cross-compilation defaults (override on CLI, e.g.:
#   make ndk ANDROID_NDK=/path/to/ndk ANDROID_ABI=arm64-v8a ANDROID_PLATFORM=24)
ANDROID_NDK      ?= $(or $(ANDROID_NDK_HOME),$(ANDROID_NDK_ROOT),$(NDK_ROOT))
ANDROID_ABI      ?= arm64-v8a
ANDROID_PLATFORM ?= 24
ANDROID_DIR      := AndroidBuild/$(ANDROID_ABI)

ifeq ($(ANDROID_ABI),arm64-v8a)
	NDK_TRIPLE := aarch64-linux-android
else ifeq ($(ANDROID_ABI),armeabi-v7a)
	NDK_TRIPLE := armv7a-linux-androideabi
else ifeq ($(ANDROID_ABI),x86_64)
	NDK_TRIPLE := x86_64-linux-android
else ifeq ($(ANDROID_ABI),x86)
	NDK_TRIPLE := i686-linux-android
else
	NDK_TRIPLE := aarch64-linux-android
endif

NDK_PREBUILT := $(firstword $(wildcard $(ANDROID_NDK)/toolchains/llvm/prebuilt/*))
NDK_CC       := $(NDK_PREBUILT)/bin/$(NDK_TRIPLE)$(ANDROID_PLATFORM)-clang
NDK_AR       := $(NDK_PREBUILT)/bin/llvm-ar

WARN_FLAGS := -Wall -Wextra -Wconversion -Wcast-qual -Wshadow -Wformat=2 -Werror -fvisibility=hidden

ifneq (,$(filter release,$(MAKECMDGOALS)))
	BUILD_DIR  := $(RELEASE_DIR)
	BUILD_TYPE := Release
	CFLAGS_OPT := -O2
	SAN_FLAGS  :=
else
	BUILD_DIR  := $(DEBUG_DIR)
	BUILD_TYPE := Debug
	CFLAGS_OPT := -g -Og
	SAN_FLAGS  := -fsanitize=address,undefined
endif

BIN   := $(BUILD_DIR)/$(APP_NAME)
MATHC := $(BUILD_DIR)/mathc

STDLIB_SOURCES := \
	Library/algebra.math \
	Library/arith.math \
	Library/biology.math \
	Library/bootstrap.math \
	Library/calculus.math \
	Library/chemistry.math \
	Library/complex.math \
	Library/conversions.math \
	Library/geom.math \
	Library/mat.math \
	Library/physics.math \
	Library/string.math \
	Library/trig.math \
	Library/units.math \
	Library/vec.math

CORE_SRCS := \
	$(wildcard Core/Code/*.c) \
	$(wildcard Core/compile/*.c) \
	$(wildcard Core/jni/*.c) \
	$(wildcard Core/lib/delegate/*.c) \
	$(wildcard Core/lib/hardcode/*.c)

TEST_SRCS := \
	$(wildcard Test/*.c)

INCLUDES := \
	-ICore/Headers \
	-ICore/compile \
	-ICore/jni \
	-ICore/lib/delegate \
	-ICore/lib/hardcode

.PHONY: clean build run test stdlib debug release ndk android clean-ndk

debug release: ;

clean:
	rm -rf $(BUILD_DIR)

clean-ndk:
	rm -rf AndroidBuild

build:
	@if [ -f $(BIN) ]; then \
		echo "Existing $(BUILD_TYPE) build found -- cleaning first"; \
		rm -rf $(BUILD_DIR); \
	fi
	@if command -v cmake >/dev/null 2>&1; then \
		cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_C_COMPILER=$(CC); \
		cmake --build $(BUILD_DIR); \
	else \
		mkdir -p $(BUILD_DIR); \
		$(CC) -std=c11 $(WARN_FLAGS) $(CFLAGS_OPT) $(SAN_FLAGS) \
			$(INCLUDES) -ITest $(CORE_SRCS) $(TEST_SRCS) -lm -pthread -o $(BIN); \
	fi

# Direct Android NDK cross-compilation (`make ndk` or `make android` / `make ndk release`)
# Produces `AndroidBuild/<ABI>/libmathengine.a`, `libmathengine.so`, and `myapp`
ndk android:
	@if [ -z "$(ANDROID_NDK)" ] || [ ! -d "$(ANDROID_NDK)" ]; then \
		echo "Error: ANDROID_NDK is not set or directory does not exist."; \
		echo "Usage: make ndk ANDROID_NDK=/path/to/android-ndk [ANDROID_ABI=arm64-v8a] [ANDROID_PLATFORM=24] [release]"; \
		exit 1; \
	fi
	mkdir -p $(ANDROID_DIR)
	@if command -v cmake >/dev/null 2>&1 && [ -f "$(ANDROID_NDK)/build/cmake/android.toolchain.cmake" ]; then \
		echo "[NDK] Configuring & building with CMake toolchain for ABI=$(ANDROID_ABI), API=$(ANDROID_PLATFORM) ($(BUILD_TYPE))..."; \
		cmake -S . -B $(ANDROID_DIR) \
			-DCMAKE_TOOLCHAIN_FILE="$(ANDROID_NDK)/build/cmake/android.toolchain.cmake" \
			-DANDROID_ABI=$(ANDROID_ABI) \
			-DANDROID_PLATFORM=android-$(ANDROID_PLATFORM) \
			-DCMAKE_BUILD_TYPE=$(BUILD_TYPE); \
		cmake --build $(ANDROID_DIR); \
	else \
		if [ ! -x "$(NDK_CC)" ]; then \
			echo "Error: NDK compiler not found at '$(NDK_CC)'"; \
			exit 1; \
		fi; \
		echo "[NDK] Compiling directly with $(NDK_CC) for ABI=$(ANDROID_ABI), API=$(ANDROID_PLATFORM)..."; \
		mkdir -p $(ANDROID_DIR)/obj; \
		for src in $(CORE_SRCS); do \
			obj="$(ANDROID_DIR)/obj/$$(basename "$$src" .c).o"; \
			"$(NDK_CC)" -std=c11 $(WARN_FLAGS) -O2 -fPIC $(INCLUDES) -c "$$src" -o "$$obj" || exit 1; \
		done; \
		"$(NDK_AR)" rcs "$(ANDROID_DIR)/libmathengine.a" $(ANDROID_DIR)/obj/*.o; \
		"$(NDK_CC)" -shared $(ANDROID_DIR)/obj/*.o -lm -o "$(ANDROID_DIR)/libmathengine.so"; \
		"$(NDK_CC)" -std=c11 $(WARN_FLAGS) -O2 -fPIE -pie $(INCLUDES) -ITest $(TEST_SRCS) "$(ANDROID_DIR)/libmathengine.a" -lm -o "$(ANDROID_DIR)/$(APP_NAME)"; \
		echo "[NDK] Built $(ANDROID_DIR)/libmathengine.a, $(ANDROID_DIR)/libmathengine.so, and $(ANDROID_DIR)/$(APP_NAME)"; \
	fi

run:
	@if [ -f $(BIN) ]; then \
		echo "$(BUILD_TYPE) binary already built -- running"; \
	else \
		$(MAKE) --no-print-directory build $(filter release,$(MAKECMDGOALS)) CC=$(CC); \
	fi
	./$(BIN)

test: build
	@if command -v ctest >/dev/null 2>&1 && [ -f $(BUILD_DIR)/CTestTestfile.cmake ]; then \
		ctest --test-dir $(BUILD_DIR) --output-on-failure; \
	else \
		./$(BIN); \
	fi

stdlib:
	@if command -v cmake >/dev/null 2>&1; then \
		if [ ! -d $(BUILD_DIR) ]; then \
			cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_C_COMPILER=$(CC); \
		fi; \
		cmake --build $(BUILD_DIR) --target stdlib; \
	else \
		mkdir -p $(BUILD_DIR); \
		$(CC) -std=c11 $(WARN_FLAGS) $(CFLAGS_OPT) $(SAN_FLAGS) \
			$(INCLUDES) $(CORE_SRCS) Library/mathc.c -lm -o $(MATHC); \
		mkdir -p precompiled; \
		./$(MATHC) precompiled/stdlib.mathc - k_stdlib_mathc $(STDLIB_SOURCES); \
	fi

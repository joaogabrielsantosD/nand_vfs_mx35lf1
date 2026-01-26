# Caminho para ESP-IDF
export IDF_PATH ?= $(firstword $(wildcard /opt/espressif/esp-idf/v5.4.2 $(HOME)/esp/v5.4.2/esp-idf))
export IDF_TOOLS_PATH ?= $(firstword $(wildcard /opt/espressif/idf-tools $(HOME)/.espressif))
PORT ?= /dev/ttyUSB0

# Alvo padrão
all: build

.PHONY: build flash monitor clean menuconfig size erase

#all:
#	bash -c "source $(IDF_PATH)/export.sh && idf.py build encrypted-flash monitor"

build:
	bash -c "source $(IDF_PATH)/export.sh && idf.py build"

flash:
	bash -c "source $(IDF_PATH)/export.sh && idf.py flash"

monitor:
	bash -c "source $(IDF_PATH)/export.sh && idf.py monitor"

program:
	bash -c "source $(IDF_PATH)/export.sh && idf.py flash monitor"

menu:
	bash -c "source $(IDF_PATH)/export.sh && idf.py menuconfig"

clean:
	bash -c "source $(IDF_PATH)/export.sh && idf.py fullclean"

size:
	bash -c "source $(IDF_PATH)/export.sh && idf.py size"

size-components:
	bash -c "source $(IDF_PATH)/export.sh && idf.py size-components"

partition:
	bash -c "source $(IDF_PATH)/export.sh && idf.py partition-table"

partition-flash:
	bash -c "source $(IDF_PATH)/export.sh && idf.py partition-table-flash"

savesdk:
	bash -c "source $(IDF_PATH)/export.sh && idf.py save-defconfig"

erase:
	bash -c "source $(IDF_PATH)/export.sh && idf.py erase-flash"

help:
	@echo ""
	@echo "Available commands:"
	@echo "  make build             - Compile the project"
	@echo "  make flash             - Compiles and uploads firmware to the board"
	@echo "  make monitor           - Open serial monitor"
	@echo "  make program           - Compiles, uploads firmware and opens serial monitor"
	@echo "  make clean             - Clean build files"
	@echo "  make size              - Show size of the firmware"
	@echo "  make size-components   - Show size of each component"
	@echo "  make partition         - Generate partition table"
	@echo "  make partition-flash   - Generate and flash partition table"
	@echo "  make savesdk           - Save current configuration to sdkconfig"
	@echo "  make erase             - Erase flash memory"
	@echo ""
	@echo "Environment variables:"
	@echo "  IDF_PATH               - ESP-IDF path (default: $(IDF_PATH))"
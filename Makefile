PYTHON ?= python3
NINJA ?= ninja
QEMU_BUILD := $(CURDIR)/qemu/build
FIRMWARE ?= ./F133-20220107_HZ-BX-B5678-UI3-V1.9-20240419-1616.bin
APPIMAGE ?= ./dist/F133-Radio-x86_64.AppImage
USB_IMAGE ?= ./run/sample-usb.img
MESON ?= meson

.PHONY: all setup configure build carve boot ui smoke smoke-touch smoke-usb appimage test-appimage test-appimage-usb

all: build

setup:
	$(PYTHON) tools/setup_qemu.py

configure: setup
	@mkdir -p "$(QEMU_BUILD)"
	@if [ ! -f "$(QEMU_BUILD)/build.ninja" ]; then \
		cd "$(QEMU_BUILD)" && "$(CURDIR)/qemu/configure" \
			--target-list=riscv64-softmmu --enable-vnc --enable-gtk --disable-modules; \
	else \
		$(MESON) configure "$(QEMU_BUILD)" -Dgtk=enabled -Dvnc=enabled -Dmodules=disabled; \
	fi

build: configure
	$(NINJA) -C "$(QEMU_BUILD)" qemu-system-riscv64

carve:
	$(PYTHON) tools/carve.py

boot: build
	run/cmdline-boot0.sh

ui: build
	run/cmdline-ui.sh

smoke: build
	$(PYTHON) tools/smoke_ui.py

smoke-touch: build
	$(PYTHON) tools/smoke_touch.py

smoke-usb: build
	$(PYTHON) tools/smoke_usb.py

appimage: build
	tools/build_appimage.sh "$(APPIMAGE)"

test-appimage: appimage
	"$(APPIMAGE)" "$(FIRMWARE)"

test-appimage-usb:
	F133_USB_IMAGE="$(USB_IMAGE)" "$(APPIMAGE)" "$(FIRMWARE)"

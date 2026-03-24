# Makefile — FluxOS bare-metal microkernel build system

# Tools
NASM     = nasm
LD       = ld
QEMU     = qemu-system-i386
GRUB_ISO = grub-mkrescue

# On macOS, we need cross-compilation tools for ELF targets
# Install: brew install i686-elf-gcc
LD_ELF := $(shell which i686-elf-ld 2>/dev/null || which x86_64-elf-ld 2>/dev/null || which ld.lld 2>/dev/null || echo ld)

# Output files
BOOT_OBJ   = boot.o
KERNEL_OBJ = kernel.o
FLAT_OBJ   = fluxos_flat.o
KERNEL_BIN = fluxos.bin
ISO_DIR    = isodir
ISO_FILE   = fluxos.iso

# Default target: single-file build (simpler, no separate boot+kernel linking)
all: $(KERNEL_BIN)

# Single-file build: everything in one asm file
$(FLAT_OBJ): fluxos_flat.asm
	$(NASM) -f elf32 -o $@ $<

$(KERNEL_BIN): $(FLAT_OBJ) linker.ld
	$(LD_ELF) -m elf_i386 -T linker.ld -o $@ $(FLAT_OBJ)

# Two-file build (boot.asm + kernel.asm)
two-file: $(BOOT_OBJ) $(KERNEL_OBJ) linker.ld
	$(LD_ELF) -m elf_i386 -T linker.ld -o $(KERNEL_BIN) $(BOOT_OBJ) $(KERNEL_OBJ)

# Assemble boot.asm → boot.o (ELF32 format)
$(BOOT_OBJ): boot.asm
	$(NASM) -f elf32 -o $@ $<

# Assemble kernel.asm → kernel.o (ELF32 format)
$(KERNEL_OBJ): kernel.asm
	$(NASM) -f elf32 -o $@ $<

# Create bootable ISO (requires grub-mkrescue + xorriso)
iso: $(KERNEL_BIN)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/
	echo 'set timeout=0'                          >  $(ISO_DIR)/boot/grub/grub.cfg
	echo 'set default=0'                          >> $(ISO_DIR)/boot/grub/grub.cfg
	echo ''                                       >> $(ISO_DIR)/boot/grub/grub.cfg
	echo 'menuentry "FluxOS" {'                   >> $(ISO_DIR)/boot/grub/grub.cfg
	echo '    multiboot2 /boot/fluxos.bin'        >> $(ISO_DIR)/boot/grub/grub.cfg
	echo '    boot'                               >> $(ISO_DIR)/boot/grub/grub.cfg
	echo '}'                                      >> $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_ISO) -o $(ISO_FILE) $(ISO_DIR)

# Run in QEMU (direct kernel boot — no ISO/GRUB needed)
run: $(KERNEL_BIN)
	$(QEMU) -kernel $(KERNEL_BIN) -m 32M -display curses 2>/dev/null || \
	$(QEMU) -kernel $(KERNEL_BIN) -m 32M -nographic || \
	qemu-system-x86_64 -kernel $(KERNEL_BIN) -m 32M -display curses 2>/dev/null || \
	qemu-system-x86_64 -kernel $(KERNEL_BIN) -m 32M -nographic

# Run with VGA window (if display is available)
run-gui: $(KERNEL_BIN)
	$(QEMU) -kernel $(KERNEL_BIN) -m 32M

# Clean build artifacts
clean:
	rm -f $(BOOT_OBJ) $(KERNEL_OBJ) $(FLAT_OBJ) $(KERNEL_BIN) $(ISO_FILE)
	rm -rf $(ISO_DIR)

.PHONY: all iso run run-gui clean

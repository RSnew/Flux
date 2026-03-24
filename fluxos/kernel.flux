// kernel.flux — FluxOS bare-metal kernel (Flux source reference)
//
// This file shows the INTENDED Flux source for the kernel.
// The companion kernel.asm is the hand-written equivalent, since
// Flux codegen does not yet support bare-metal / freestanding targets.
//
// When Flux gains freestanding codegen support, this file will compile
// directly to a bootable kernel object.

// ═══════════════════════════════════════════════════════════
// VGA Text Mode Constants
// ═══════════════════════════════════════════════════════════
conf VGA_BASE   = 0xB8000
conf VGA_COLS   = 80
conf VGA_ROWS   = 25
conf WHITE      = 0x0F
conf GREEN      = 0x0A
conf CYAN       = 0x0B
conf YELLOW     = 0x0E

// ═══════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════
var cursor_row = 0
var cursor_col = 0

// ═══════════════════════════════════════════════════════════
// VGA Output Functions
// ═══════════════════════════════════════════════════════════

// Clear the screen — fill VGA buffer with spaces
func clear_screen() {
    var vga = addr(VGA_BASE)
    var i = 0
    while i < VGA_COLS * VGA_ROWS {
        vga[i * 2]     = 0x20     // space character
        vga[i * 2 + 1] = WHITE    // white on black
        i = i + 1
    }
    cursor_row = 0
    cursor_col = 0
}

// Write a single character at the cursor position
func put_char(ch, color) {
    var vga = addr(VGA_BASE)
    var offset = (cursor_row * VGA_COLS + cursor_col) * 2
    vga[offset]     = ch
    vga[offset + 1] = color

    cursor_col = cursor_col + 1
    if cursor_col >= VGA_COLS {
        cursor_col = 0
        cursor_row = cursor_row + 1
        if cursor_row >= VGA_ROWS {
            scroll_screen()
        }
    }
}

// Print a null-terminated string
func print_string(msg, color) {
    var i = 0
    while msg[i] != 0 {
        put_char(msg[i], color)
        i = i + 1
    }
}

// Print string followed by newline
func print_line(msg, color) {
    print_string(msg, color)
    newline()
}

// Move to next line
func newline() {
    cursor_col = 0
    cursor_row = cursor_row + 1
    if cursor_row >= VGA_ROWS {
        scroll_screen()
    }
}

// Scroll screen up by one line
func scroll_screen() {
    var vga = addr(VGA_BASE)
    var i = 0
    // Copy rows 1..24 to rows 0..23
    while i < VGA_COLS * (VGA_ROWS - 1) * 2 {
        vga[i] = vga[i + VGA_COLS * 2]
        i = i + 1
    }
    // Clear last row
    while i < VGA_COLS * VGA_ROWS * 2 {
        vga[i] = 0x20      // space
        vga[i + 1] = WHITE
        i = i + 2
    }
    cursor_row = VGA_ROWS - 1
}

// ═══════════════════════════════════════════════════════════
// Keyboard Input (port I/O via inline asm)
// ═══════════════════════════════════════════════════════════

func read_key() {
    // In real Flux bare-metal:
    // asm { "in al, 0x64" }   // check status
    // asm { "in al, 0x60" }   // read scancode
    // ... scancode to ASCII conversion ...
    return 0
}

// ═══════════════════════════════════════════════════════════
// Kernel Main Entry Point
// ═══════════════════════════════════════════════════════════

func kernel_main() {
    clear_screen()

    print_line("FluxOS v0.1 - Flux Language Bare Metal Kernel", CYAN)
    print_line("================================================", CYAN)
    newline()
    print_line("Type 'help' for available commands.", GREEN)
    newline()

    // Command loop
    while true {
        print_string("fluxos> ", YELLOW)
        // ... read input, dispatch commands ...
        // (see kernel.asm for the full implementation)
    }
}

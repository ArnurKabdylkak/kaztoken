/*
 * linker.ld — LPC1768 bare-metal linker script
 *
 * Memory layout (from datasheet):
 *   Flash  : 0x0000_0000 – 0x0007_FFFF  (512 kB)
 *   SRAM   : 0x1000_0000 – 0x1000_7FFF  (32 kB, local CPU SRAM)
 *   AHB SRAM: 0x2007_C000 – 0x2007_FFFF (16 kB, bank 0)
 *             0x2008_0000 – 0x2008_3FFF  (16 kB, bank 1)
 *
 * We use only Flash + the main 32 kB SRAM.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 512K
    SRAM  (rwx) : ORIGIN = 0x10000000, LENGTH = 32K
}

/* Stack grows downward from top of SRAM */
_stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

ENTRY(Reset_Handler)

SECTIONS
{
    /* ---- Interrupt vector table must be at 0x0000_0000 ---- */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* ---- Code + read-only data ---- */
    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* ---- Initialised data (LMA in Flash, VMA in SRAM) ---- */
    _data_load = LOADADDR(.data);

    .data :
    {
        _data_start = .;
        *(.data*)
        . = ALIGN(4);
        _data_end = .;
    } > SRAM AT > FLASH

    /* ---- Zero-initialised data ---- */
    .bss :
    {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > SRAM

    /* ---- Heap starts after BSS ---- */
    _heap_start = _bss_end;
    _heap_end   = _stack_top - 0x400;   /* leave 1 kB guard */
}

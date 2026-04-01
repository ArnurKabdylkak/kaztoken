/* startup_stm32f411.s — Minimal startup for STM32F411 bare-metal glitcher */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .global _start
    .global Reset_Handler
    .global Default_Handler

/* ---- Stack ---- */
    .section .stack, "aw", %nobits
    .align 3
    .space 4096          /* 4 KB stack */
stack_top:

/* ---- Vector table ---- */
    .section .vectors, "a", %progbits
    .align 2
    .word stack_top          /*  0: Initial SP              */
    .word Reset_Handler      /*  1: Reset                   */
    .word Default_Handler    /*  2: NMI                     */
    .word Default_Handler    /*  3: HardFault               */
    .word Default_Handler    /*  4: MemManage               */
    .word Default_Handler    /*  5: BusFault                */
    .word Default_Handler    /*  6: UsageFault              */
    .word 0                  /*  7: Reserved                */
    .word 0                  /*  8: Reserved                */
    .word 0                  /*  9: Reserved                */
    .word 0                  /* 10: Reserved                */
    .word Default_Handler    /* 11: SVCall                  */
    .word Default_Handler    /* 12: Debug Monitor           */
    .word 0                  /* 13: Reserved                */
    .word Default_Handler    /* 14: PendSV                  */
    .word Default_Handler    /* 15: SysTick                 */
    /* External interrupts 0..85 */
    .rept 86
    .word Default_Handler
    .endr

/* ---- Reset handler ---- */
    .section .text
    .thumb_func
    .type Reset_Handler, %function
Reset_Handler:
    /* Copy .data from FLASH to SRAM */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata
copy_data:
    cmp r0, r1
    bge zero_bss
    ldr r3, [r2], #4
    str r3, [r0], #4
    b copy_data

    /* Zero .bss */
zero_bss:
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r3, #0
bss_loop:
    cmp r0, r1
    bge call_main
    str r3, [r0], #4
    b bss_loop

call_main:
    bl main
    b .

/* ---- Default handler (infinite loop) ---- */
    .thumb_func
    .type Default_Handler, %function
Default_Handler:
    b .
    .size Default_Handler, .-Default_Handler
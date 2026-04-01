#include <stdint.h>

#define PCONP       (*(volatile uint32_t*)0x400FC0C4)
#define PCLKSEL0    (*(volatile uint32_t*)0x400FC1A8)
#define PINSEL0     (*(volatile uint32_t*)0x4002C000)
#define FIO2DIR     (*(volatile uint32_t*)0x2009C040)
#define FIO2SET     (*(volatile uint32_t*)0x2009C058)
#define FIO2CLR     (*(volatile uint32_t*)0x2009C05C)
#define U0THR       (*(volatile uint32_t*)0x4000C000)
#define U0DLL       (*(volatile uint32_t*)0x4000C000)
#define U0DLM       (*(volatile uint32_t*)0x4000C004)
#define U0FCR       (*(volatile uint32_t*)0x4000C008)
#define U0LCR       (*(volatile uint32_t*)0x4000C00C)
#define U0LSR       (*(volatile uint32_t*)0x4000C014)
#define U0FDR       (*(volatile uint32_t*)0x4000C028)

#define LED_DS1     (1u << 2)
#define LED_DS2     (1u << 3)
#define LED_DS3     (1u << 4)
#define LED_DS5     (1u << 5)
#define LED_ALL     (LED_DS1 | LED_DS2 | LED_DS3 | LED_DS5)

#define BOOTROM_BASE  0x1FFF0000u
#define BOOTROM_SIZE  (8u * 1024u)   /* 8 КБ */
#define HEX_COLS      16             /* байт на строку */

/* ~1 сек @ IRC 4 MHz */
static void delay_3s(void) {
    volatile uint32_t i = 3000000UL;
    while (i--) __asm__("nop");
}

static void uart0_init(void) {
    PCONP    |= (1u << 3);
    PCLKSEL0  = (PCLKSEL0 & ~(3u << 6)) | (1u << 6); /* PCLK = CCLK = 4 MHz */
    PINSEL0   = (PINSEL0  & ~(0xFFu << 4)) | (0x55u << 4); /* P0.2=TXD0, P0.3=RXD0 */
    U0LCR = 0x83;           /* DLAB=1, 8N1 */
    U0DLL = 26; U0DLM = 0; /* 9600 @ 4 MHz */
    U0FDR = (1u << 4) | 0u;/* FDR off */
    U0LCR = 0x03;           /* DLAB=0 */
    U0FCR = 0x07;
}

static void putc_(char c) {
    while (!(U0LSR & (1u << 5)));
    U0THR = c;
}

static void puts_(const char *s) {
    while (*s) {
        if (*s == '\n') putc_('\r');
        putc_(*s++);
    }
}





static void leds_init(void) {
    FIO2DIR |= LED_ALL;  /* выходы */
    FIO2SET  = LED_ALL;  /* все выключены (active LOW) */
}

int main(void) {
    uart0_init();
    leds_init();

    puts_("LPC1768 LED sequencer started\n");
    puts_("DS1=P2.2  DS2=P2.3  DS3=P2.4  DS5=P2.5  (active LOW)\n\n");

    /* ---- Дамп BootROM 0x1FFF0000, 8 КБ ---- */
    // puts_("=== BootROM dump: 0x1FFF0000, 8192 bytes ===\n\n");
    // dump_hex(BOOTROM_BASE, BOOTROM_SIZE);
    // puts_("\n=== BootROM dump complete ===\n\n");

    /* ---- Основной цикл: LED-секвенсор ---- */
    const uint32_t masks[4] = { LED_DS1, LED_DS2, LED_DS3, LED_DS5 };
    const char    *names[4] = { "DS1 (P2.2)", "DS2 (P2.3)",
                                 "DS3 (P2.4)", "DS5 (P2.5)" };

    while (1) {
        for (int i = 0; i < 4096; i++) {
            FIO2SET = LED_ALL;          /* все OFF */
            FIO2CLR = masks[i % 4];     /* текущий ON */
            puts_(names[i % 4]);
            puts_(" ON\n");
            delay_3s();
        }
    }
}
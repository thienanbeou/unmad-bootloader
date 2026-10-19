/*
 * MAD60 HE bootloader: an open replacement for the vendor "MAD60 Bootloader"
 * in sector 0 (0x08000000, 16 KB).
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * Unofficial drop-in for the vendor bootloader (reverse engineered
 * 2026-10-18, see keyboards/mad60he/NOTES.md in the libhmk MAD60 HE fork):
 * same boot rule, the
 * vendor's USB ID 373B:2028 and 64-byte vendor HID reports, and the same flash
 * protocol, so fwtool.py (libhmk fork, keyboards/mad60he/tools) and the
 * vendor updater work unchanged. The ID
 * is reused only for that compatibility; the strings say what this is.
 *
 * Boot rule (vendor FUN_08003744): the word at 0x2001FFFC survives a reset but
 * not a power cycle. Not BOOT_MAGIC: set it and start the app (the app clears
 * it; libhmk does in board_init). BOOT_MAGIC (the app asked for the bootloader,
 * or crashed before clearing it): clear it and stay in USB mode until AA 96.
 * Ours also stays when Esc is held while the app would start (v4), so any app,
 * however broken, can be replaced; except right after AA 96, which always
 * starts the app.
 *
 * Packets: 64 bytes, byte 63 = OR of bytes 0..62. The reply echoes the packet
 * with byte 63 = that OR, inverted when the command failed.
 *   AA 01 <u32 BE pages of 2 KB> <u32 BE base>     start a session
 *   AA 02 <u32 BE sectors>                         erase sectors 5.. (at most 3)
 *   AA 03 <u32 BE page addr> <idx> <len> <last> <data[len <= 54]>
 *                                                  idx 0 opens a page, last programs it
 *   AA 96                                          reset into the app
 * The first 8 bytes of the image (initial SP and reset vector) are programmed
 * after the last page, so a partly written app never looks valid.
 *
 * Differences from the vendor code: writes are limited to the app sectors 5-7
 * (the vendor bootloader writes wherever a packet says, itself included),
 * every word is read back, a page whose length is not a multiple of 4 is padded
 * with 0xFF instead of losing its last bytes, and the app starts from a real
 * reset instead of a jump out of a running USB stack.
 */

#include <stdbool.h>
#include <string.h>

#include "stm32f4xx.h"
#include "tusb.h"

#define APP_BASE 0x08020000u
#define APP_END 0x08080000u // sectors 5-7: the vendor protocol erases no more
#define APP_FIRST_SECTOR 5u
#define APP_SECTORS 3u
#define PAGE_SIZE 0x800u
#define CHUNK_MAX 54u

#define BOOT_FLAG (*(volatile uint32_t *)0x2001FFFCu)
#define BOOT_MAGIC 0x96969696u
// Left by AA 96: start the app even with Esc held (not BOOT_MAGIC, so the
// vendor rule and the apps read it as "start the app")
#define BOOT_START_APP 0x41505021u

//--------------------------------------------------------------------+
// Boot
//--------------------------------------------------------------------+

static bool app_valid(void) {
  const uint32_t *vt = (const uint32_t *)APP_BASE;
  // Vendor rule: initial SP in the first 128 KB of RAM. Also require a Thumb
  // reset vector inside the app area.
  return (vt[0] & 0x2FFE0000u) == 0x20000000u && (vt[1] & 1u) &&
         vt[1] > APP_BASE && vt[1] < APP_END;
}

__attribute__((noreturn)) static void app_start(void) {
  const uint32_t *vt = (const uint32_t *)APP_BASE;
  SCB->VTOR = APP_BASE;
  __DSB();
  __ISB();
  __asm volatile("msr msp, %0\n\tbx %1" : : "r"(vt[0]), "r"(vt[1]));
  __builtin_unreachable();
}

//--------------------------------------------------------------------+
// Esc check
//--------------------------------------------------------------------+

// Raw 12-bit levels fall when a key is pressed: about 1860-2060 at rest,
// 1110-1160 bottomed out (all 61 keys, 2026-10-17). Below 500 is no reading
// (the stock firmware's rule). 1495 = libhmk's Bootmagic level 2600 inverted.
#define ESC_PRESSED_MIN 500u
#define ESC_PRESSED_MAX 1495u
// Readings about 1 ms apart that must all be pressed
#define ESC_SAMPLES 20u

// Columns, active low (MAD60HE_NOTES 4b): col 0 = PA8 is Esc's
#define COLS_A 0x9F00u // PA8-PA12, PA15
#define COLS_B 0x0078u // PB3-PB6
#define COLS_C 0x1C00u // PC10-PC12
#define COLS_D 0x0004u // PD2

static void delay(uint32_t n) {
  for (volatile uint32_t i = 0; i < n; i++)
    ;
}

/** @brief MODER value with every pin in `pins` set to `mode` */
static uint32_t moder(uint32_t moder, uint32_t pins, uint32_t mode) {
  for (uint32_t i = 0; i < 16; i++)
    if (pins & (1u << i))
      moder = (moder & ~(3u << (2 * i))) | (mode << (2 * i));
  return moder;
}

/** @brief One reading of row 0 (PA3, ADC1 channel 3) */
static uint32_t esc_read(void) {
  ADC1->SR = 0;
  ADC1->CR2 |= ADC_CR2_SWSTART;
  while (!(ADC1->SR & ADC_SR_EOC))
    ;
  return ADC1->DR;
}

/**
 * @brief Whether Esc is held, read the way libhmk scans it
 *
 * Runs on the reset clock (HSI 16 MHz) and puts every peripheral it used back
 * to its reset state, so the app still starts from a clean chip. A level
 * outside the pressed band at any reading means not held: a misread starts the
 * app, the normal case costs one reading.
 */
static bool esc_held(void) {
  const uint32_t ahb1enr = RCC->AHB1ENR, apb2enr = RCC->APB2ENR;
  RCC->AHB1ENR = ahb1enr | RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN |
                 RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN;
  RCC->APB2ENR = apb2enr | RCC_APB2ENR_ADC1EN;
  (void)RCC->APB2ENR;

  // Every column inactive (high) before it becomes an output, then column 0
  GPIOA->BSRR = COLS_A;
  GPIOB->BSRR = COLS_B;
  GPIOC->BSRR = COLS_C;
  GPIOD->BSRR = COLS_D;
  GPIOA->MODER = moder(moder(GPIOA->MODER, COLS_A, 1), 1u << 3, 3);
  GPIOB->MODER = moder(GPIOB->MODER, COLS_B, 1);
  GPIOC->MODER = moder(GPIOC->MODER, COLS_C, 1);
  GPIOD->MODER = moder(GPIOD->MODER, COLS_D, 1);
  GPIOA->BSRR = GPIO_BSRR_BR8;

  // ADC clock 8 MHz (PCLK2 / 2), 56-cycle sampling, one conversion of ch 3
  ADC1->SMPR2 = 3u << ADC_SMPR2_SMP3_Pos;
  ADC1->SQR1 = 0;
  ADC1->SQR3 = 3;
  ADC1->CR2 = ADC_CR2_ADON;
  delay(100); // ADC wake-up and column settle (libhmk: 10 us)

  bool held = true;
  for (uint32_t i = 0; held && i < ESC_SAMPLES; i++) {
    const uint32_t v = esc_read();
    held = v >= ESC_PRESSED_MIN && v <= ESC_PRESSED_MAX;
    if (held && i + 1 < ESC_SAMPLES)
      delay(2000);
  }

  ADC1->CR2 = 0;
  RCC->APB2RSTR = RCC_APB2RSTR_ADCRST;
  RCC->APB2RSTR = 0;
  RCC->AHB1RSTR = RCC_AHB1RSTR_GPIOARST | RCC_AHB1RSTR_GPIOBRST |
                  RCC_AHB1RSTR_GPIOCRST | RCC_AHB1RSTR_GPIODRST;
  RCC->AHB1RSTR = 0;
  RCC->APB2ENR = apb2enr;
  RCC->AHB1ENR = ahb1enr;
  return held;
}

//--------------------------------------------------------------------+
// Clocks and USB
//--------------------------------------------------------------------+

/**
 * @brief Bring the chip to a known state, then run from HSE at 168 MHz with a
 * 48 MHz USB clock (PLLM 16, N 336, P 2, Q 7: the stock and libhmk settings)
 *
 * The stock firmware jumps here with its clocks running (1.06 FUN_08026a80),
 * so this does not assume reset values. The data cache stays off: it could
 * return stale flash words when reading back what was just programmed.
 */
static void clock_init(void) {
  SysTick->CTRL = 0;
  for (uint32_t i = 0; i < 8; i++) {
    NVIC->ICER[i] = 0xFFFFFFFFu;
    NVIC->ICPR[i] = 0xFFFFFFFFu;
  }
  RCC->AHB1RSTR = 0xFFFFFFFFu;
  RCC->AHB1RSTR = 0;
  RCC->AHB2RSTR = 0xFFFFFFFFu;
  RCC->AHB2RSTR = 0;
  RCC->APB1RSTR = 0xFFFFFFFFu;
  RCC->APB1RSTR = 0;
  RCC->APB2RSTR = 0xFFFFFFFFu;
  RCC->APB2RSTR = 0;

  RCC->CR |= RCC_CR_HSION;
  while (!(RCC->CR & RCC_CR_HSIRDY))
    ;
  RCC->CFGR = 0;
  while (RCC->CFGR & RCC_CFGR_SWS)
    ;
  RCC->CR &= ~RCC_CR_PLLON;
  while (RCC->CR & RCC_CR_PLLRDY)
    ;

  RCC->APB1ENR |= RCC_APB1ENR_PWREN;
  PWR->CR |= PWR_CR_VOS;
  RCC->CR |= RCC_CR_HSEON;
  while (!(RCC->CR & RCC_CR_HSERDY))
    ;
  RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSE | (16u << RCC_PLLCFGR_PLLM_Pos) |
                 (336u << RCC_PLLCFGR_PLLN_Pos) | (7u << RCC_PLLCFGR_PLLQ_Pos);
  RCC->CR |= RCC_CR_PLLON;
  while (!(RCC->CR & RCC_CR_PLLRDY))
    ;
  FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_5WS;
  RCC->CFGR = RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2 | RCC_CFGR_SW_PLL;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
    ;
}

/**
 * @brief OTG_HS core with its embedded full-speed PHY on PB14 (D-) / PB15
 * (D+), AF12, no VBUS sensing (as libhmk's board_usb_init)
 */
static void usb_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
  GPIOB->OSPEEDR |= 0xFu << 28;
  GPIOB->PUPDR &= ~(0xFu << 28);
  GPIOB->AFR[1] = (GPIOB->AFR[1] & ~0xFF000000u) | 0xCC000000u;
  GPIOB->MODER = (GPIOB->MODER & ~(0xFu << 28)) | (0xAu << 28);

  RCC->AHB1ENR |= RCC_AHB1ENR_OTGHSEN;
  USB_OTG_HS->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;
  USB_OTG_HS->GCCFG &= ~(USB_OTG_GCCFG_VBUSBSEN | USB_OTG_GCCFG_VBUSASEN);
}

void OTG_HS_IRQHandler(void) { tud_int_handler(BOARD_TUD_RHPORT); }

//--------------------------------------------------------------------+
// Flash
//--------------------------------------------------------------------+

static bool flash_wait(void) {
  while (FLASH->SR & FLASH_SR_BSY)
    ;
  const uint32_t err = FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR |
                                    FLASH_SR_PGPERR | FLASH_SR_PGSERR);
  FLASH->SR = err | FLASH_SR_EOP;
  return err == 0;
}

static void flash_unlock(void) {
  if (FLASH->CR & FLASH_CR_LOCK) {
    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;
  }
  flash_wait();
}

static bool flash_erase_sector(uint32_t sector) {
  flash_unlock();
  FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos);
  FLASH->CR |= FLASH_CR_STRT;
  const bool ok = flash_wait();
  FLASH->CR = FLASH_CR_LOCK;
  return ok;
}

/** @brief Program and read back `len` bytes (a multiple of 4) */
static bool flash_program(uint32_t addr, const uint8_t *src, uint32_t len) {
  bool ok = true;
  flash_unlock();
  FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
  for (uint32_t i = 0; ok && i < len; i += 4) {
    uint32_t w;
    memcpy(&w, src + i, 4);
    *(volatile uint32_t *)(addr + i) = w;
    ok = flash_wait() && *(volatile uint32_t *)(addr + i) == w;
  }
  FLASH->CR = FLASH_CR_LOCK;
  return ok;
}

//--------------------------------------------------------------------+
// Protocol
//--------------------------------------------------------------------+

static struct {
  uint32_t pages, pages_done; // from AA 01
  uint32_t page_addr, fill;   // page being received
  bool page_open;
  uint8_t vectors[8]; // first 8 bytes of the image, programmed last
  bool have_vectors;
} st;
static uint8_t page[PAGE_SIZE] __attribute__((aligned(4)));
static volatile bool reset_pending;

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool page_program(void) {
  const uint32_t addr = st.page_addr;
  uint32_t len = (st.fill + 3u) & ~3u;
  // addr >= APP_END first: APP_END - addr wraps for pages above the app area
  if (addr < APP_BASE || addr >= APP_END || addr % 4 || len > APP_END - addr)
    return false;
  memset(page + st.fill, 0xFF, len - st.fill);

  uint32_t skip = 0;
  if (addr == APP_BASE) {
    if (len < 8)
      return false;
    memcpy(st.vectors, page, 8);
    st.have_vectors = true;
    skip = 8;
  }
  if (!flash_program(addr + skip, page + skip, len - skip))
    return false;

  if (++st.pages_done == st.pages && st.have_vectors)
    return flash_program(APP_BASE, st.vectors, 8);
  return true;
}

static bool command(const uint8_t *p) {
  switch (p[1]) {
  case 0x01:
    memset(&st, 0, sizeof(st));
    st.pages = be32(p + 2);
    return be32(p + 6) == APP_BASE && st.pages <= (APP_END - APP_BASE) / PAGE_SIZE;

  case 0x02: {
    uint32_t n = be32(p + 2);
    if (n > APP_SECTORS)
      n = APP_SECTORS;
    for (uint32_t i = 0; i < n; i++)
      if (!flash_erase_sector(APP_FIRST_SECTOR + i))
        return false;
    return true;
  }

  case 0x03: {
    const uint32_t len = p[7];
    if (len > CHUNK_MAX)
      return false;
    if (p[6] == 0) {
      st.page_addr = be32(p + 2);
      st.fill = 0;
      st.page_open = true;
    }
    if (!st.page_open || st.fill + len > PAGE_SIZE)
      return false;
    memcpy(page + st.fill, p + 9, len);
    st.fill += len;
    if (!p[8])
      return true;
    st.page_open = false;
    return page_program();
  }

  case 0x96:
    reset_pending = true;
    return true;

  default:
    return true; // the vendor code echoes unknown commands too
  }
}

/**
 * @brief Handle one 64-byte packet, write the reply to `out`
 *
 * Not static so the emulator check can call it directly.
 */
__attribute__((noinline, used)) void boot_packet(const uint8_t *in, uint8_t *out) {
  uint8_t sum = 0;
  for (uint32_t i = 0; i < 63; i++)
    sum |= in[i];
  // Not a command: echo it, as the vendor code does. A command whose checksum
  // does not match fails, so the host never takes it as written.
  bool ok = in[0] != 0xAA;
  if (!ok && in[63] == sum)
    ok = command(in);
  memcpy(out, in, 63);
  sum |= in[63];
  out[63] = ok ? sum : (uint8_t)~sum;
}

//--------------------------------------------------------------------+
// USB descriptors (copied from the vendor bootloader's RAM image, except the
// manufacturer and product strings)
//--------------------------------------------------------------------+

static const uint8_t desc_device[] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x3B, 0x37,
    0x28, 0x20, 0x00, 0x02, 0x01, 0x02, 0x03, 0x01,
};

static const uint8_t desc_report[] = {
    0x06, 0xFF, 0x00, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x25, 0xFF,
    0x75, 0x08, 0x95, 0x40, 0x09, 0x01, 0x81, 0x02, 0x95, 0x40, 0x09,
    0x01, 0x91, 0x02, 0x95, 0x01, 0x09, 0x01, 0xB1, 0x02, 0xC0,
};

static const uint8_t desc_config[] = {
    0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0xE0, 0x32,       // config
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,       // interface
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, sizeof(desc_report), 0x00, // HID
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,                   // EP1 IN
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x01,                   // EP1 OUT
};

static const char *const strings[] = {
    NULL, "Unmad (unofficial)", "Unmad bootloader v4", "000000000001",
};

uint8_t const *tud_descriptor_device_cb(void) { return desc_device; }

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { return desc_config; }

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { return desc_report; }

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  static uint16_t buf[40];
  uint32_t n = 1;
  if (index == 0)
    buf[1] = 0x0409;
  else if (index < TU_ARRAY_SIZE(strings))
    for (const char *s = strings[index]; *s && n < TU_ARRAY_SIZE(buf); s++)
      buf[n++] = (uint8_t)*s;
  else
    return NULL;
  if (index == 0)
    n = 2;
  buf[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n));
  return buf;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  static uint8_t reply[64];
  if (report_id != 0 || report_type != HID_REPORT_TYPE_OUTPUT || bufsize != 64)
    return;
  boot_packet(buffer, reply);
  tud_hid_report(0, reply, sizeof(reply));
}

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+

int main(void) {
  if (BOOT_FLAG != BOOT_MAGIC) {
    const bool requested = BOOT_FLAG == BOOT_START_APP;
    BOOT_FLAG = BOOT_MAGIC;
    if (app_valid() && (requested || !esc_held()))
      app_start();
  } else {
    BOOT_FLAG = 0;
  }

  clock_init();
  usb_init();
  tud_init(BOARD_TUD_RHPORT);
  __enable_irq();

  while (1) {
    tud_task();
    // AA 96: reset once the host has read the reply. The next boot starts the
    // app, even with Esc held.
    if (reset_pending && tud_hid_ready()) {
      delay(1000000);
      BOOT_FLAG = BOOT_START_APP;
      NVIC_SystemReset();
    }
  }
}

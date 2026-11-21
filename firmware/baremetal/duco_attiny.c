/*
  DUCO-S1a miner for the ATtiny45 and ATtiny85.

  These chips have no TWI module and no UART, so they cannot run an Arduino
  sketch at all: the bus has to be driven pin by pin, and the only way to hand a
  board a job is to hang it off a rig master (firmware/i2c_rig) on the bus.

  Everything the chip needs is in two files: this driver, and attiny_hash.c,
  the search. There is no Arduino core, no Wire and no Serial here, which is
  why an ATtiny45 with 4 KB of flash can hold a miner at all.

  The ATtiny25 is the chip that cannot. It has 2 KB of flash and 128 bytes of
  RAM, and a DUCO-S1a search needs 104 bytes of RAM before the search loop's
  own stack frame - the 16 word schedule window is 64 bytes, the previous hash
  20, the target digest 20 - plus about 60 more for the registers the round
  function cannot fit in the AVR's 24. The build for attiny25 compiles and
  measures 3,886 bytes of flash and 114 bytes of globals, so it is nearly twice
  the flash and a third more RAM than that chip has. The ATtiny45 is the smallest
  ATtiny this miner runs on, and it fits with 182 bytes of flash and 72 bytes of
  RAM to spare.

  USER SETTINGS
  -------------
  BUS_ADDRESS  The number the rig master calls this board by, 1 to 126.
  SDA_PIN, SCL_PIN, LED_PIN
               Pins on PORTB. Pin change interrupts on these chips only exist
               for PB0 to PB5, so the bus has to be on that port.
  F_CPU        The clock the chip is fused for. A brand new ATtiny45/85 runs
               at 1 MHz; most boards people buy have been set to 8 MHz. The
               build script takes -DF_CPU=8000000 the same way an Arduino sketch
               does, so set it here if you build by hand.

  Build and flash:

      ./build.sh attiny45
      avrdude -c usbtiny -p attiny45 -P /dev/ttyUSB0 \
              -U flash:w:duco_attiny45.hex

  The programmer id depends on what you are using: usbtiny for an ATMega-style
  USB stick, arduino for an old Arduino doing the ISP bridge, avr for an
  AVRISP mkII.

  Wiring: SDA and SCL to the same two lines the rig master uses, with the
  master's pull-ups still on the bus, GND to GND. Run the bus slowly - the rig
  master's RIG_BUS_KHZ wants to be 10 or less for these boards, because every
  bit of every byte is handled in an interrupt that has to keep up with the
  clock.
*/

#ifndef F_CPU
#define F_CPU 8000000UL
#endif

#define BUS_ADDRESS 1
#define SDA_PIN     0   /* PB0 */
#define SCL_PIN     1   /* PB1 */
#define LED_PIN     4   /* PB4 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

#include "attiny_hash.h"

/* The reply line goes in the schedule window, which is empty between searches
   and 64 bytes long; the longest line a share can make is 43 characters. */
#define REPLY ((uint8_t *)attiny_window)

#define SDA_BIT (1u << SDA_PIN)
#define SCL_BIT (1u << SCL_PIN)
#define LED_BIT (1u << LED_PIN)

/* Phases of one I2C transaction as seen by the slave. */
enum { PH_IDLE, PH_ADDR, PH_ACK, PH_RX, PH_TX, PH_MACK };

/* The search takes 104 of the chip's bytes before the stack is counted, so the
   state of the bus is packed into as few of them as it will bear: ten bytes
   between the two lines, the reply position, the job being parsed, the work
   limit and the clock. */
static volatile uint8_t pins;      /* bit0 SCL level, bit1 SDA level,
                                      bit2 searching, bits3-7 blink counter */
static volatile uint8_t state;     /* bits0-2 phase, bits3-6 bits of the byte in
                                      progress, bit7 the master is reading */
static volatile uint8_t rx;        /* the byte being put together */
static volatile uint8_t replyPos;
static volatile uint8_t replyLen;
static volatile uint8_t parse;     /* bits0-1 which field, bits2-7 characters of it */
static volatile uint16_t work;     /* difficulty, then the nonce limit */
static volatile uint16_t ticks;    /* timer 0 overflows, for elapsed time */

#define PHASE() (state & 7u)
#define SET_PHASE(p) (state = (uint8_t)((state & 0xF8u) | (p)))
#define BITCOUNT() (uint8_t)(state >> 3)
#define SET_BITCOUNT(b) (state = (uint8_t)((state & 0x07u) | ((b) << 3)))
#define READING() (uint8_t)(state >> 7)
#define SET_READING(r) (state = (uint8_t)((state & 0x7Fu) | ((r) << 7)))
#define FIELD() (uint8_t)(parse & 3u)
#define SET_FIELD(f) (parse = (uint8_t)((parse & 0xFCu) | (f)))
#define SEEN() (uint8_t)(parse >> 2)
#define SET_SEEN(n) (parse = (uint8_t)((parse & 3u) | ((n) << 2)))

/* Open drain: a one is a released line pulled up by the bus, a zero is driven
   low. Driving SDA high would fight the master, so it never happens. */
static void sdaLow(void) { DDRB |= SDA_BIT; PORTB &= ~SDA_BIT; }
static void sdaRelease(void) { DDRB &= ~SDA_BIT; PORTB |= SDA_BIT; }

static uint8_t replyByte(void) {
  return replyPos < replyLen ? REPLY[replyPos] : (uint8_t)'\n';
}

static uint8_t nibble(char c) {
  return (uint8_t)(c >= 'a' ? c - 'a' + 10 : c - '0');
}

/* Take one character of "prev,target,difficulty". The job is never held as
   text: the bytes go straight into the search's own arrays. */
static void feed(char c) {
  uint8_t seen = SEEN();

  if (c == ',') {
    if (FIELD() == 0) {
      attiny_target[0] = 0;
      attiny_target[1] = 0;
      attiny_target[2] = 0;
      attiny_target[3] = 0;
      attiny_target[4] = 0;
    }
    SET_FIELD((uint8_t)(FIELD() + 1));
    SET_SEEN(0);
    work = 0;
  } else if (c == '\n') {
    uint8_t field = FIELD();
    SET_FIELD(0);
    SET_SEEN(0);
    if (field >= 2 && work) {
      work = (uint16_t)(work * 100u + 1u);   /* the nonce range for this job */
      attiny_finish_target();
      replyLen = 0;
      replyPos = 0;
      pins |= 4;                            /* searching */
    } else {
      work = 0;                             /* a job line with no difficulty */
    }
  } else if (FIELD() == 0 && seen < 40) {
    uint8_t n = nibble(c);
    if (seen & 1) attiny_prev[seen >> 1] |= n;
    else attiny_prev[seen >> 1] = (uint8_t)(n << 4);
    SET_SEEN((uint8_t)(seen + 1));
  } else if (FIELD() == 1 && seen < 40) {
    attiny_target[seen >> 3] |=
      (uint32_t)nibble(c) << (24 - 4 * (seen & 7));
    SET_SEEN((uint8_t)(seen + 1));
  } else if (FIELD() == 2 && c >= '0' && c <= '9') {
    work = (uint16_t)(work * 10 + (c - '0'));
  }
}

ISR(PCINT0_vect) {
  uint8_t sda = (uint8_t)((PINB >> SDA_PIN) & 1u);
  uint8_t scl = (uint8_t)((PINB >> SCL_PIN) & 1u);
  uint8_t old = pins;
  pins = (uint8_t)((old & 0xFC) | (uint8_t)(sda << 1) | scl);
  uint8_t changed = old ^ pins;

  /* SDA moving while the clock is high is a start or a stop. Nothing else
     moves SDA while the clock is high: a slave only drives SDA on the falling
     edge of the clock. */
  if (scl && (changed & 2)) {
    if (sda) {
      SET_PHASE(PH_IDLE);
      sdaRelease();
    } else {
      SET_PHASE(PH_ADDR);
      SET_BITCOUNT(0);
      rx = 0;
      sdaRelease();
    }
    return;
  }

  if (scl && (changed & 1)) {
    /* Rising clock: read the bit the other end is holding on SDA. */
    if (PHASE() == PH_ADDR || PHASE() == PH_RX) {
      rx = (uint8_t)((rx << 1) | sda);
      if (BITCOUNT() == 7) {
        SET_BITCOUNT(0);
        if (PHASE() == PH_ADDR) {
          SET_READING(rx & 1u);
          SET_PHASE(((rx >> 1) == BUS_ADDRESS) ? PH_ACK : PH_IDLE);
        } else {
          SET_PHASE(PH_ACK);
        }
      } else {
        SET_BITCOUNT((uint8_t)(BITCOUNT() + 1));
      }
    } else if (PHASE() == PH_MACK) {
      if (sda) {
        SET_PHASE(PH_IDLE);           /* the master is done reading */
      } else {
        replyPos++;                   /* acknowledged: hand over the next byte */
        SET_BITCOUNT(0);
        SET_PHASE(PH_TX);
      }
    }
  } else if (!scl && (changed & 1)) {
    /* Falling clock: whatever is driven now is what the next rising edge sees. */
    if (PHASE() == PH_ACK) {
      sdaLow();                        /* acknowledge the byte just received */
      if (READING()) {
        SET_BITCOUNT(0);
        SET_PHASE(PH_TX);
      } else {
        SET_PHASE(PH_RX);
        if (rx == '?') replyPos = 0;
        else if (!(pins & 4)) feed((char)rx);
      }
    } else if (PHASE() == PH_TX) {
      if (BITCOUNT() < 8) {
        uint8_t byte = replyByte();
        if ((byte >> (7 - BITCOUNT())) & 1u) sdaRelease();
        else sdaLow();
        SET_BITCOUNT((uint8_t)(BITCOUNT() + 1));
      } else {
        sdaRelease();                  /* let the master send its acknowledge */
        SET_PHASE(PH_MACK);
      }
    }
  }
}

/* Timer 0 runs free at one count per cycle, so 256 counts is one overflow and
   the overflow count gives the elapsed time without a divide in the search. */
ISR(TIMER0_OVF_vect) { ticks++; }

/* Timer 1 is only 8 bits on these chips, so with the 1024 prescaler a compare
   match comes every 262,144 clock cycles: 32.768 ms at 8 MHz and 262 ms at
   1 MHz. Counting the matches gives a blink close to the 200 ms beat the
   Arduino sketches use, and the count rides in the spare bits of the pin state
   so it costs no RAM. */
#define BLINK_MATCHES ((F_CPU / 5UL + 262143UL) / 262144UL)

ISR(TIMER1_COMPA_vect) {
  if (!(pins & 4)) return;               /* only blink while a job is running */
  uint8_t count = (uint8_t)(pins >> 3) + 1u;
  if (count < BLINK_MATCHES) {
    pins = (uint8_t)((pins & 0x07u) | (count << 3));
  } else {
    pins &= 0x07u;
    PORTB ^= LED_BIT;
  }
}

/* The signature row is not reachable by a plain flash read: the bit that lets
   a program read it is SIGRD on an ATmega and RSIG on an ATtiny, and avr-libc
   only knows the ATmega name. */
static uint8_t signatureByte(uint16_t address) {
  uint8_t result;
  __asm__ __volatile__ (
    "sts %1, %2\n\t"
    "lpm %0, Z\n\t"
    : "=r" (result)
    : "i" (_SFR_MEM_ADDR(SPMCSR)),
      "r" ((uint8_t)((1u << SPMEN) | (1u << RSIG))),
      "z" (address)
  );
  return result;
}

static uint8_t putUint(uint8_t *out, uint8_t pos, uint32_t value) {
  char digits[10];
  uint8_t n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  while (n) out[pos++] = (uint8_t)digits[--n];
  return pos;
}

static void ledFlash(void) {
  PORTB |= LED_BIT;
  _delay_ms(120);
  PORTB &= ~LED_BIT;
  _delay_ms(120);
  PORTB |= LED_BIT;
  _delay_ms(120);
  PORTB &= ~LED_BIT;
}

int main(void) {
  DDRB &= (uint8_t)~(SDA_BIT | SCL_BIT);
  PORTB |= SDA_BIT | SCL_BIT;          /* pull-ups on the two bus lines */
  DDRB |= LED_BIT;
  PORTB &= ~LED_BIT;

  pins = 3;                            /* an idle bus has both lines high */
  PCMSK = SDA_BIT | SCL_BIT;
  GIMSK |= (1 << PCIE);

  TCCR0B = 1;                          /* no prescaler: one tick per cycle */
  TIMSK |= (1 << TOIE0);

  TCCR1 = (1 << CTC1) | (1 << CS13);   /* timer 1 divided by 1024 */
  OCR1A = 255;
  TCNT1 = 0;
  TIMSK |= (1 << OCIE1A);

  sei();

  for (;;) {
    if (!(pins & 4)) continue;

    uint16_t limit = work;
    TCNT0 = 0;
    ticks = 0;

    uint32_t nonce = attiny_mine(limit);

    /* Timer 0 is 8 bit, so elapsed is the overflow count plus the current
       count, in cycles. */
    uint32_t cycles = ((uint32_t)ticks << 8) + TCNT0;
    uint32_t elapsed = cycles / (F_CPU / 1000000UL);
    if (!elapsed) elapsed = 1;

    uint8_t pos = putUint(REPLY, 0, nonce);
    REPLY[pos++] = ',';
    pos = putUint(REPLY, pos, elapsed);
    REPLY[pos++] = ',';
    REPLY[pos++] = 'D';
    REPLY[pos++] = 'U';
    REPLY[pos++] = 'C';
    REPLY[pos++] = 'O';
    REPLY[pos++] = 'I';
    REPLY[pos++] = 'D';
    /* These chips have no unique board id, so the identity is the device
       signature plus the bus address: every board on a rig is different
       because no two of them share an address. */
    for (uint8_t i = 0; i < 7; i++) {
      uint8_t value = signatureByte((uint16_t)(0x3F00 + i));
      REPLY[pos++] = (uint8_t)attiny_hex_char(value >> 4);
      REPLY[pos++] = (uint8_t)attiny_hex_char(value & 0x0fu);
    }
    REPLY[pos++] = (uint8_t)attiny_hex_char(BUS_ADDRESS >> 4);
    REPLY[pos++] = (uint8_t)attiny_hex_char(BUS_ADDRESS & 0x0fu);
    REPLY[pos++] = '\n';

    replyLen = pos;
    replyPos = 0;
    pins &= 0x07u;                       /* no longer searching */

    ledFlash();
  }
}

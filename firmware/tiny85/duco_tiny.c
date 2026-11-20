/*
  Bare-metal DUCO-S1a miner for AVR boards with only a few KB of flash.

  Why not an Arduino sketch: on a 328P the Arduino core, Wire and Serial add
  about 5 KB before any of the mining code is counted, and the I2C glue is
  another 2 KB. A board with 8 KB of program flash has no room for them, so
  this file drives the TWI and UART registers itself and uses the small search
  core from firmware/tiny_core.

  Two ways to hand it a job, exactly like the Arduino sketches:

    serial - "prevHash,targetHash,difficulty\n" in, "nonce,elapsed,DUCOID\n"
             out with the nonce and the elapsed time written as binary digits.
             This needs a chip with a UART, which the ATmega8, the ATmega16 and
             the 168/328/1280/2560 family all have. An ATtiny85 has neither a
             UART nor a TWI module, so there is no way to hand it a job at all
             and the build for it is a size check only.

    I2C    - the DuinoCoinI2C rig protocol (https://github.com/ricaun/DuinoCoinI2C):
             the master pushes the job one byte per transmission and polls for
             the answer one byte at a time. A board that is still searching
             answers every poll with a bare newline.

  Everything is guarded by what the chip has: the TWI driver is compiled in
  only for chips with a TWI module, the UART driver only for chips with a
  UART. firmware/tiny85/README.md has the measured flash and RAM figures for
  ATmega8, ATmega16 and ATtiny85.

  Build and flash:

      ./build.sh atmega328p
      avrdude -c arduino -p atmega328p -P /dev/ttyUSB0 \
              -U flash:w:duco_tiny_atmega328p.hex

  (with -C pointing at your avrdude.conf if avrdude cannot find one on its own;
  the programmer id is `arduino`, the one the Arduino bootloader speaks, not
  `avr`.)

  Wire the bus to the TWI pins of the board, which are not always the pins a
  sketch library calls analog: A4/A5 (PC4/PC5) on a 328P or an ATmega8 board,
  PC0/PC1 on an ATmega16, digital 20/21 on a Mega.
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/boot.h>
#include <stdint.h>
#include <string.h>

/* <util/delay.h> needs F_CPU, which is spelled out just below. */
#ifndef F_CPU
#define F_CPU 16000000UL
#endif
#include <util/delay.h>

#include "tiny_hash.h"

/* Rounded so the nearest baud rate is chosen: at 16 MHz this is 111,111 baud
   for 115200, a 3.5% error, which is the same value the Arduino core picks.
   Rounding down to 125,000 baud is an 8.5% error and serial links do not
   reliably work at that much drift. */
#define BAUD 115200UL
#define UART_DIV ((F_CPU / 16UL + BAUD / 2UL) / BAUD - 1UL)

/* Whether this chip has a TWI module at all. Building with -DDUCO_NO_I2C
   leaves the bus out entirely, which is what an ATtiny85 needs: it has no
   TWI hardware, only USI, and the driver below will not run on it. */
#if defined(TWBR) && !defined(DUCO_NO_I2C)
#define HAVE_TWI 1
#endif

/* Whether this chip has a UART at all. The 168/328/1280/2560 class names its
   registers with a 0 suffix, an ATmega8 or ATmega16 without one. */
#if defined(UDR0) || defined(UDR)
#define HAVE_UART 1
/* Defined further down; the bus code wants to say what it found. */
static void uart_putc(char c);
static void uart_puts(const char *s);
static void uart_putdec(uint8_t value);
#endif

/* A chip with neither a TWI module nor a UART (an ATtiny85) has no way to be
   handed a job at all, so on that chip the whole job path is compiled out and
   the build is a fit check rather than a miner. Marking the helpers that only
   some configurations reach keeps GCC from calling them dead code. */
#define MAY_BE_UNUSED __attribute__((unused))

/* A job is two 40 character hashes, a difficulty and two commas: 86 bytes
   plus the newline. One job wide would be enough for a board that is never
   sent a second job while it is still working on the first, so the ring holds
   a little over one. Ring indices wrap by masking, so the size is a power of
   two. Serial and I2C bytes both land in this ring, so a job arriving on
   either path is parsed by the same code. */
#define JOB_LIMIT 128
#define RESP_LIMIT 48

static char jobIn[JOB_LIMIT];
static volatile uint8_t jobHead;    /* written by the interrupts */
static volatile uint8_t jobTail;    /* read by the main loop     */
static volatile uint8_t jobOverflow;
static volatile uint8_t searching;  /* set while a nonce search is running */

MAY_BE_UNUSED static char i2cReply[RESP_LIMIT];
static volatile uint8_t i2cReplyLen;
static volatile uint8_t i2cReplyNext;

MAY_BE_UNUSED static char serialReply[80];  /* up to 16 binary digits for the nonce, 32 for the elapsed microseconds, the 22 character id and the separators */

static char prevHash[41];
static char targetHash[41];
static char ducoid[23];

/* ---------------- how long a search took ---------------- */

static volatile uint16_t tickOverflow;

ISR(TIMER1_OVF_vect) {
  tickOverflow++;
}

/* The overflow counter is 16 bit, so the 32 bit tick count this returns repeats
   every 2^32 ticks - about 4.5 minutes at 16 MHz. That is longer than any
   search the protocol can ask for, and the differences taken from it are
   unsigned 32 bit subtractions, so a wrap in the middle of a measurement still
   gives the right answer. */
#if defined(TCNT1H)
  /* Timer 1 is 16 bit here, so counting cycles costs one interrupt every
     65,536 cycles - 4,096 us at 16 MHz - and the elapsed time is exact. */
  #define TIMER_PRESCALER 1
  static uint32_t readTicks(void) {
    /* Reading TCNT1 is two byte loads. An overflow landing between them, or
       between that and the read of tickOverflow, would put the count off by up
       to 65,536 ticks, about 4 ms at 16 MHz. Taking the snapshot with the
       overflow interrupt off costs a handful of cycles, and this is only called
       between jobs, never inside a hash. */
    cli();
    uint32_t ticks = (((uint32_t)tickOverflow << 16) | TCNT1);
    sei();
    return ticks;
  }
#else
  /* Timer 1 is 8 bit on the ATtiny85. Counting every cycle would interrupt
     62,500 times a second, so there it ticks at one eighth of the clock: an
     interrupt every 128 us, and still an exact elapsed time. */
  #define TIMER_PRESCALER 8
  static uint32_t readTicks(void) {
    cli();
    uint32_t ticks = (((uint32_t)tickOverflow << 8) | TCNT1);
    sei();
    return ticks;
  }
#endif

static uint32_t microsecondsSince(uint32_t startTicks) {
  return (readTicks() - startTicks) * TIMER_PRESCALER / (F_CPU / 1000000UL);
}

static void timer_start(void) {
#if defined(TCCR1B)
  TCCR1B = TIMER_PRESCALER;
#elif defined(TCCR1)
  TCCR1 = TIMER_PRESCALER;
#endif
#if defined(TIMSK1)
  TIMSK1 |= _BV(TOV1);
#elif defined(TIMSK)
  TIMSK |= _BV(TOV1);
#endif
}

/* ---------------- the job line ---------------- */

static char hexNibble(char c) {
  return (c >= '0' && c <= '9') ? (char)(c - '0') : (char)((c & 0x0f) + 9);
}

MAY_BE_UNUSED static char hexChar(uint8_t value) {
  return value < 10 ? (char)('0' + value) : (char)('A' + value - 10);
}

static void hexToBytes(const char *hex, uint8_t *raw) {
  for (uint8_t i = 0, j = 0; j < TINY_HASH_LEN; i += 2, j++)
    raw[j] = (uint8_t)(((uint8_t)hexNibble(hex[i]) << 4) |
                       (uint8_t)hexNibble(hex[i + 1]));
}

MAY_BE_UNUSED static uint8_t appendDecimal(char *out, uint8_t pos, uint32_t value) {
  static char tmp[10];
  uint8_t n = 0;
  do {
    tmp[n++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value);
  while (n) out[pos++] = tmp[--n];
  return pos;
}

/* The serial protocol of the AVR miner sends the nonce and the elapsed time as
   a string of '0' and '1' characters - that is what the PC side parses. Over
   I2C the rig masters read the line as plain numbers. */
MAY_BE_UNUSED static uint8_t appendBinary(char *out, uint8_t pos, uint32_t value) {
  uint8_t bit = 31;
  while (bit && !((value >> bit) & 1u)) bit--;
  for (;;) {
    out[pos++] = (char)('0' + ((value >> bit) & 1u));
    if (!bit) break;
    bit--;
  }
  return pos;
}

MAY_BE_UNUSED static void pushJobByte(char c) {
  /* Bytes arriving while the board is busy with a job it already took are
     dropped rather than stored. Two masters writing into one ring is the only
     way a job line can end up spliced onto somebody else's bytes, which reads
     as a valid job with the wrong hashes and sends the board hunting for a
     nonce that does not exist. */
  if (searching) return;
  if ((uint8_t)(jobHead - jobTail) >= JOB_LIMIT - 1) {
    /* A half stored job would splice onto the bytes that follow it and be
       parsed as a job with the wrong hashes, so nothing in the ring is read
       until the main loop clears it. */
    jobOverflow = 1;
    return;
  }
  jobIn[jobHead++] = c;
}

/* Take one newline terminated job out of the ring. Returns 0 while the job is
   still arriving. */
static uint32_t takeJob(void) {
  if (jobOverflow) {
    jobOverflow = 0;
    jobTail = jobHead;
  }

  /* Snapshot how much the interrupts have handed over: they keep appending
     while this scans, and comparing the scan position against a length that
     has grown since the scan started is what makes a job still in transit look
     like a whole one. */
  uint8_t tail = jobTail;
  uint8_t head = jobHead;
  uint8_t end = tail;
  while (end != head && jobIn[end] != '\n') end++;
  if (end == head) return 0;

  uint8_t len = (uint8_t)(end - tail);
  uint8_t pos = 0, n = 0;

  while (pos < len && n < 40 && jobIn[(uint8_t)(tail + pos)] != ',')
    prevHash[n++] = jobIn[(uint8_t)(tail + pos++)];
  jobTail = (uint8_t)(end + 1);
  if (n != 40) return 0;
  while (pos < len && jobIn[(uint8_t)(tail + pos)] == ',') pos++;

  n = 0;
  while (pos < len && n < 40 && jobIn[(uint8_t)(tail + pos)] != ',')
    targetHash[n++] = jobIn[(uint8_t)(tail + pos++)];
  if (n != 40) return 0;
  while (pos < len && jobIn[(uint8_t)(tail + pos)] == ',') pos++;

  uint32_t difficulty = 0;
  uint8_t digits = 0;
  while (pos < len && jobIn[(uint8_t)(tail + pos)] >= '0' &&
         jobIn[(uint8_t)(tail + pos)] <= '9') {
    difficulty = difficulty * 10u + (uint32_t)(jobIn[(uint8_t)(tail + pos)] - '0');
    digits++;
    pos++;
  }
  if (digits == 0) return 0;

  prevHash[40] = 0;
  targetHash[40] = 0;
  return difficulty;
}

static uint32_t search(uint32_t difficulty, uint32_t *elapsed) {
  /* Above 655 the nonce range no longer fits the search the protocol allows. */
  if (difficulty > 655) {
    *elapsed = 0;
    return 0;
  }

  uint8_t digest[TINY_HASH_LEN];
  hexToBytes(targetHash, digest);

  uint32_t started = readTicks();
  uint32_t nonce = tiny_mine(prevHash, digest, difficulty * 100u + 1u);
  *elapsed = microsecondsSince(started);
  return nonce;
}

/* ---------------- the bus ---------------- */

#if defined(HAVE_TWI)

#include <util/twi.h>

#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
  /* digital 20 is SDA (PD1) and digital 21 is SCL (PD0) */
  #define TWI_DDR DDRD
  #define TWI_PORT PORTD
  #define TWI_PIN PIND
  #define TWI_SDA_BIT 1
  #define TWI_SCL_BIT 0
#elif defined(__AVR_ATmega16__)
  /* the ATmega16 puts the bus on PC0 (SCL) and PC1 (SDA), not on the
     RXD/TXD pair that shares port D */
  #define TWI_DDR DDRC
  #define TWI_PORT PORTC
  #define TWI_PIN PINC
  #define TWI_SDA_BIT 1
  #define TWI_SCL_BIT 0
#else
  /* ATmega8, ATmega168/328 class: A4 is SDA (PC4), A5 is SCL (PC5) */
  #define TWI_DDR DDRC
  #define TWI_PORT PORTC
  #define TWI_PIN PINC
  #define TWI_SDA_BIT 4
  #define TWI_SCL_BIT 5
#endif

#if defined(HAVE_UART)
/* What the bus looked like when the address scan gave up, kept for the one
   line the board prints about it. */
static uint8_t busStatus;
static uint8_t busLines;
#endif

static void twi_ack(int ack) {
  /* TWIE has to be written every time: TWCR is not a set/clr register, so a
   write that leaves the interrupt-enable bit out turns the bus interrupt off
   for good. */
  TWCR = ack ? (_BV(TWINT) | _BV(TWEN) | _BV(TWIE) | _BV(TWEA))
             : (_BV(TWINT) | _BV(TWEN) | _BV(TWIE));
}

static uint8_t nextReplyByte(void) {
  if (i2cReplyNext < i2cReplyLen) return (uint8_t)i2cReply[i2cReplyNext++];
  return '\n';
}

#if defined(DUCO_DEBUG)
/* Bus event counters for the serial trace: how many times the board was
   addressed to write, how many job bytes arrived, how many polls came in. */
static volatile uint16_t busWrites, busData, busStops;
static volatile uint16_t busReads, busReadData, busReadNack, busUnknown;
#endif

ISR(TWI_vect) {
#if defined(DUCO_DEBUG)
  /* Counts of the bus events, printed by the mining loop every second so a
   board with no debugger can be watched from the serial port. */
  switch (TW_STATUS) {
    case TW_SR_SLA_ACK:    busWrites++;    break;
    case TW_SR_DATA_ACK:   busData++;     break;
    case TW_SR_STOP:       busStops++;    break;
    case TW_ST_SLA_ACK:    busReads++;    break;
    case TW_ST_DATA_ACK:   busReadData++; break;
    case TW_ST_DATA_NACK:  busReadNack++; break;
    case TW_ST_LAST_DATA:  busReadNack++; break;
    default:               busUnknown++;  break;
  }
#endif

  switch (TW_STATUS) {
    case TW_SR_SLA_ACK:          /* somebody addressed us to write */
    case TW_SR_GCALL_ACK:
    case TW_SR_ARB_LOST_SLA_ACK:
      twi_ack(1);
      break;

    case TW_SR_DATA_ACK:         /* a job byte arrived */
    case TW_SR_GCALL_DATA_ACK:
      pushJobByte((char)TWDR);
      twi_ack(1);
      break;

    case TW_SR_STOP:
      twi_ack(1);
      break;

    case TW_ST_SLA_ACK:          /* somebody polled us for the answer */
    case TW_ST_ARB_LOST_SLA_ACK:
      TWDR = nextReplyByte();
      twi_ack(1);
      break;

    case TW_ST_DATA_ACK:         /* the master wants another byte */
      TWDR = nextReplyByte();
      twi_ack(1);
      break;

    case TW_ST_DATA_NACK:        /* it has all it asked for */
    case TW_ST_LAST_DATA:
      twi_ack(1);
      break;

    default:
      /* An unexpected state still needs TWINT set, or the TWI module sits
         stalled waiting for the handler to answer it. */
      twi_ack(1);
      break;
  }
}

/* Every step of a master transaction waits for TWINT to come back. On a bus
   that is shorted, held low by another device, or has no pull-ups at all that
   condition never happens, and an unbounded spin would leave the board stuck
   before it ever mined. A bounded spin costs about 5 ms per step. */
static int twi_wait(void) {
  for (uint16_t i = 0; i < 20000; i++)
    if (TWCR & _BV(TWINT)) return 1;
  return 0;
}

static void twi_stop(void) {
  TWCR = _BV(TWINT) | _BV(TWSTO) | _BV(TWEN);
  for (uint16_t i = 0; i < 20000 && (TWCR & _BV(TWSTO)); i++) ;
}

/* A start condition needs SDA and SCL both high. Probing a bus that another
   device is currently holding low does nothing except jam it for everybody, so
   the lines are checked before a probe is attempted. */
static int lines_idle(void) {
  uint8_t both = _BV(TWI_SDA_BIT) | _BV(TWI_SCL_BIT);
  return (TWI_PIN & both) == both;
}

/* Sends START, the address and a STOP. Returns 1 when a slave answered, 0 when
   nobody did (so the address is free), and -1 when the bus never got far
   enough to send an address at all.

   A START that never completes is more than a failed probe: the TWI module
   keeps asking for the start condition and holds SDA low while it waits, which
   blocks every other device on the bus. The module has to be switched off
   before giving up, otherwise the board jams the bus and nothing on it - this
   board included - can talk. */
static int twi_probe_address(uint8_t candidate) {
  if (!lines_idle()) return -1;

  TWCR = _BV(TWINT) | _BV(TWSTA) | _BV(TWEN);
  if (!twi_wait()) {
    TWCR = 0;
    return -1;
  }

  TWDR = (uint8_t)(candidate << 1);
  TWCR = _BV(TWINT) | _BV(TWEN);
  if (!twi_wait()) {
    TWCR = 0;
    return -1;
  }

  uint8_t status = TW_STATUS;
  twi_stop();
  TWCR = 0;
  return status == TW_MT_SLA_ACK;
}

/* A transmission to a free address gets no acknowledge, so the first address
   nobody answers to is the one this board takes. A bus that is busy at the
   instant we try it is retried a few times before the board gives up on the
   bus and mines over serial alone. */
static uint8_t twi_pick_address(void) {
  TWBR = (uint8_t)((F_CPU / 100000UL - 16) / 2);   /* about 100 kHz */
  TWSR = 0;

  for (uint8_t pass = 0; pass < 3; pass++) {
    for (uint8_t candidate = 1; candidate < 127; candidate++) {
      int answered = twi_probe_address(candidate);
      if (answered < 0) break;      /* busy: start the scan again later */
      if (!answered) return candidate;
    }
    _delay_ms(150);
  }
  return 0;
}

static uint8_t twi_begin(void) {
  /* Input, with the internal pull-ups on: a 328P board has no external
     resistors on these lines, a Mega 2560 has them onboard already. */
  TWI_DDR &= ~(_BV(TWI_SDA_BIT) | _BV(TWI_SCL_BIT));
  TWI_PORT |= (_BV(TWI_SDA_BIT) | _BV(TWI_SCL_BIT));

  uint8_t address = twi_pick_address();

#if defined(HAVE_UART)
  if (!address) {
    busStatus = TW_STATUS;
    busLines = TWI_PIN;
  }
#endif

  if (!address) return 0;

  TWAR = (uint8_t)((address << 1) | 1);              /* bit 0 selects us */
  TWCR = _BV(TWINT) | _BV(TWEN) | _BV(TWIE) | _BV(TWEA);
  return address;
}

static void twi_submit(uint32_t nonce, uint32_t elapsed) {
  uint8_t pos = appendDecimal(i2cReply, 0, nonce);
  i2cReply[pos++] = ',';
  pos = appendDecimal(i2cReply, pos, elapsed);
  i2cReply[pos++] = ',';
  for (uint8_t i = 0; ducoid[i] && pos < RESP_LIMIT - 1; i++)
    i2cReply[pos++] = ducoid[i];
  i2cReply[pos++] = '\n';
  i2cReplyNext = 0;
  i2cReplyLen = pos;
}

#if defined(DUCO_DEBUG) && defined(HAVE_UART)
static void uart_printdec(uint32_t value) {
  char digits[12];
  uint8_t n = appendDecimal(digits, 0, value);
  digits[n] = 0;
  uart_puts(digits);
}

static void trace_bus(void) {
  uart_puts("bus: writes "); uart_printdec(busWrites);
  uart_puts(" data "); uart_printdec(busData);
  uart_puts(" stops "); uart_printdec(busStops);
  uart_puts(" polls "); uart_printdec(busReads);
  uart_puts(" poll bytes "); uart_printdec(busReadData);
  uart_puts(" nacks "); uart_printdec(busReadNack);
  uart_puts(" other "); uart_printdec(busUnknown);
  uart_putc('\n');
}
#endif

#endif /* HAVE_TWI */

/* ---------------- the serial port ---------------- */

#if defined(HAVE_UART)

#if defined(UDR0)
  /* ATmega168/328/1280/2560 */
  static void uart_init(void) {
    UBRR0 = (uint16_t)UART_DIV;
    /* RXCIE0 is what makes a received byte raise the interrupt. RXEN0 alone
     only samples the line: the byte lands in UDR0 and nothing else happens. */
    UCSR0B = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
  }

  static void uart_putc(char c) {
    while (!(UCSR0A & _BV(UDRE0))) ;
    UDR0 = (uint8_t)c;
  }

#if defined(USART_RX_vect)
  ISR(USART_RX_vect) { pushJobByte((char)UDR0); }
#endif
#else
  /* ATmega8 and ATmega16 spell the same interface without the 0 suffix.
     UBRRH and UCSRC share one I/O address, so UBRRH has to be written
     before UCSRC. */
  static void uart_init(void) {
    UBRRH = 0;
    UBRRL = (uint8_t)UART_DIV;
    UCSRB = _BV(RXEN) | _BV(TXEN) | _BV(RXCIE);
    UCSRC = _BV(UCSZ1) | _BV(UCSZ0);
  }

  static void uart_putc(char c) {
    while (!(UCSRA & _BV(UDRE))) ;
    UDR = (uint8_t)c;
  }

#if defined(USART_RXC_vect)
  ISR(USART_RXC_vect) { pushJobByte((char)UDR); }
#endif
#endif

static void uart_puts(const char *s) {
  while (*s) uart_putc(*s++);
}

MAY_BE_UNUSED static void uart_putdec(uint8_t value) {
  char digits[4];
  uint8_t n = appendDecimal(digits, 0, value);
  for (uint8_t i = 0; i < n; i++) uart_putc(digits[i]);
}

#endif

#if defined(DUCO_DEBUG) && defined(HAVE_UART)
/* There is no debugger on a bare-metal board, so a build with -DDUCO_DEBUG
   prints every job it has parsed before it starts searching it. */
static void debug_dump_job(uint32_t difficulty) {
  uart_puts("job ");
  uart_puts(prevHash);
  uart_putc(' ');
  uart_puts(targetHash);
  uart_puts(" diff ");
  char digits[12];
  uint8_t n = appendDecimal(digits, 0, difficulty);
  digits[n] = 0;
  uart_puts(digits);
  uart_putc('\n');
}
#endif

/* ---------------- the board's own id ---------------- */

static void build_ducoid(uint8_t address) {
  ducoid[0] = 'D'; ducoid[1] = 'U'; ducoid[2] = 'C'; ducoid[3] = 'O';
  ducoid[4] = 'I'; ducoid[5] = 'D';

  /* The 168/328 class chips carry an eight byte device id in the boot
     signature row, which is what the rigs use as the miner's identity. Chips
     without one (ATmega8, ATmega16, ATtiny85) have nothing to read, and clone
     boards sometimes ship with the row blank, so several boards can end up
     reporting the same id. Two boards reporting the same id are counted as one
     miner by the pool. */
#if defined(__SPM_REG) && (defined(__AVR_ATmega328P__) || defined(__AVR_ATmega328__) || \
                           defined(__AVR_ATmega168P__) || defined(__AVR_ATmega168__))
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t b = boot_signature_byte_get(0x0E + i);
    ducoid[6 + 2 * i] = hexChar(b >> 4);
    ducoid[7 + 2 * i] = hexChar(b & 0x0f);
  }
#else
  memcpy(ducoid + 6, "0000000000000000", 16);
#endif

  /* The bus address the board claimed goes in the last byte: same length, and
     every board on a rig is distinguishable from the others whatever its
     signature row holds. A board with no address keeps the plain id. */
  if (address) {
    ducoid[20] = hexChar(address >> 4);
    ducoid[21] = hexChar(address & 0x0f);
  }
  ducoid[22] = 0;
}

/* ---------------- main ---------------- */

#if defined(HAVE_TWI)
static void report_address(uint8_t address) {
#if defined(HAVE_UART)
  if (address) {
    uart_puts("I2C slave address: ");
    uart_putdec(address);
    uart_putc('\n');
  } else {
    uart_puts("I2C: no free bus address. Bus status ");
    uart_putdec(busStatus);
    uart_puts(", TWI lines read ");
    uart_putdec(busLines);
    uart_puts(". Mining over serial only, the bus is retried every five seconds.\n");
  }
#endif
}
#endif

int main(void) {
  timer_start();

#if defined(HAVE_UART)
  uart_init();
#endif

#if defined(HAVE_TWI)
  uint8_t address = twi_begin();
  build_ducoid(address);
  report_address(address);
  /* A bus that was busy at the instant the board came up is not a reason to
   give up on it for the life of the board, so the scan is tried again every
   five seconds until an address is claimed. */
  uint32_t retryAt = readTicks() + 5UL * F_CPU;
#if defined(DUCO_DEBUG) && defined(HAVE_UART)
  uint32_t traceAt = readTicks();
#endif
#else
  build_ducoid(0);
#endif

  sei();

  for (;;) {
#if defined(HAVE_TWI)
    if (!address && (uint32_t)(readTicks() - retryAt) < (1UL << 31)) {
      address = twi_begin();
      if (address) {
        build_ducoid(address);
        report_address(address);
      }
      retryAt = readTicks() + 5UL * F_CPU;
    }
#if defined(DUCO_DEBUG) && defined(HAVE_UART)
    if ((uint32_t)(readTicks() - traceAt) < (1UL << 31) &&
        (uint32_t)(readTicks() - traceAt) >= F_CPU) {
      traceAt = readTicks();
      trace_bus();
    }
#endif
#endif

    uint32_t difficulty = takeJob();
    if (!difficulty) continue;

    searching = 1;

#if defined(DUCO_DEBUG) && defined(HAVE_UART)
    debug_dump_job(difficulty);
#endif

    uint32_t elapsed;
    MAY_BE_UNUSED uint32_t nonce = search(difficulty, &elapsed);
    searching = 0;

#if defined(HAVE_TWI)
    twi_submit(nonce, elapsed);
#endif
#if defined(HAVE_UART)
    uint8_t pos = appendBinary(serialReply, 0, nonce);
    serialReply[pos++] = ',';
    pos = appendBinary(serialReply, pos, elapsed);
    serialReply[pos++] = ',';
    for (uint8_t i = 0; i < 22; i++) serialReply[pos++] = ducoid[i];
    serialReply[pos++] = '\n';
    serialReply[pos] = 0;
    uart_puts(serialReply);
#endif
  }
}

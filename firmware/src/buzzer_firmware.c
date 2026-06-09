/*
 * noknok Buzzer Module Firmware  v3.1
 * CH32V003J4M6 (SOP-8)  |  Stack: cnlohr/ch32fun
 *
 * ── What changed from v2 ────────────────────────────────────────────────
 *   v2 had a hardcoded I2C address (0x45).
 *   v3 adds dynamic address discovery (enumeration):
 *   - All modules boot with I2C OFF, counting a UID-seeded backoff timer.
 *   - When the timer fires, I2C turns on at the staging address (0x7F).
 *   - The Conductor reads the 10-byte UID+type+CRC response.
 *   - The Conductor writes a new unique address. Module switches and is ready.
 *   This allows multiple identical modules on the same bus.
 *
 * ── Hardware ─────────────────────────────────────────────────────────────
 *   PA1 (pin 2)  TIM1 CH2 → MMBT3904 NPN → MLT-8530 buzzer
 *   PC1 (pin 3)  I2C SDA
 *   PC2          I2C SCL
 *
 * ── Enumeration protocol ─────────────────────────────────────────────────
 *   Staging address : 0x7F
 *   Runtime address : assigned by Conductor (0x08–0x77)
 *
 *   1. Conductor reads 10 bytes from 0x7F:
 *        [UID 0..7]   64-bit hardware UID (little-endian)
 *        [0x01]       MODULE_TYPE (buzzer)
 *        [CRC8]       CRC8 of bytes 0–8
 *
 *   2. Conductor writes to 0x7F:
 *        [0x1D, new_addr]   ASSIGN — module switches to new_addr
 *
 * ── Normal operation protocol ────────────────────────────────────────────
 *   I2C address: assigned at runtime
 *
 *   Pico WRITES:
 *     [0x00]                     STOP
 *     [0x01, fH, fL, dur, vol]   PLAY NOTE
 *     [0x02, id]                 PLAY TUNE (1–5)
 *
 *   Pico READS:
 *     1 byte: 0x01=playing, 0x00=idle
 */

#include "ch32fun.h"
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ENUM_ADDR       0x7F    /* staging address — all modules start here */
#define MODULE_TYPE     0x01    /* 0x01 = buzzer */
#define TIM_CLK_HZ      1000000UL

#define CMD_STOP        0x00
#define CMD_PLAY_NOTE   0x01
#define CMD_PLAY_TUNE   0x02
#define CMD_ENTER_BOOTLOADER 0xB0   /* reset into the I2C bootloader for OTA update */
#define REG_ASSIGN_ADDR 0x1D

/* Bootloader handoff cell — top 16 B of RAM, reserved by app.ld (stack ends
 * below it). Writing this magic then warm-resetting drops the module into the
 * shared noknok I2C bootloader (flash mode at 0x7E). SRAM survives a warm reset.
 * Magic + address MUST match noknok_bootloader. */
#define BL_MAGIC_CELL   (*(volatile uint32_t *)0x200007F0U)
#define BL_MAGIC_ENTER  0x6E6B4231U   /* "nkB1" */

/* CH32V003 64-bit hardware UID (ESIG_UNIID1 + ESIG_UNIID2 = 8 bytes) */
#define UID_ADDR        ((volatile uint8_t*)0x1FFFF7E8)
#define UID_LEN         8


/* ═══════════════════════════════════════════════════════════════════════════
 * TUNE DATA  (stored in flash, zero RAM cost)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct { uint16_t freq; uint16_t dur_ms; } Note;

#define N_REST  0
#define N_C4    262
#define N_Cs4   277
#define N_D4    294
#define N_Ds4   311
#define N_E4    330
#define N_F4    349
#define N_Fs4   370
#define N_G4    392
#define N_Gs4   415
#define N_A4    440
#define N_Bb4   466
#define N_B4    494
#define N_C5    523
#define N_Cs5   554
#define N_D5    587
#define N_E5    659
#define N_G5    784
#define N_A5    880
#define N_C6    1047

static const Note tune_nokia[] = {
    {N_E5,125},{N_D5,125},{N_Fs4,250},{N_Gs4,250},
    {N_Cs5,125},{N_B4,125},{N_D4,250},{N_E4,250},
    {N_B4,125},{N_A4,125},{N_Cs4,250},{N_E4,250},
    {N_A4,500},
};
static const Note tune_happy_birthday[] = {
    {N_C4,200},{N_C4,200},{N_D4,400},{N_C4,400},{N_F4,400},{N_E4,800},{N_REST,200},
    {N_C4,200},{N_C4,200},{N_D4,400},{N_C4,400},{N_G4,400},{N_C5,800},{N_REST,200},
    {N_C4,200},{N_C4,200},{N_C5,400},{N_A4,400},{N_F4,200},{N_E4,200},{N_D4,800},{N_REST,200},
    {N_Bb4,200},{N_Bb4,200},{N_A4,400},{N_F4,400},{N_G4,400},{N_F4,800},
};
static const Note tune_beep_ok[]    = {{N_A4,80},{N_REST,40},{N_C5,80}};
static const Note tune_beep_error[] = {{N_D4,150},{N_REST,60},{N_D4,150}};
static const Note tune_startup[]    = {{N_C5,100},{N_E5,100},{N_G5,100},{N_C6,250}};

typedef struct { const Note *notes; uint8_t count; } Tune;
#define TUNE_COUNT 5
static const Tune tune_table[TUNE_COUNT + 1] = {
    {0,0},
    {tune_nokia,          sizeof(tune_nokia)          / sizeof(Note)},
    {tune_happy_birthday, sizeof(tune_happy_birthday) / sizeof(Note)},
    {tune_beep_ok,        sizeof(tune_beep_ok)        / sizeof(Note)},
    {tune_beep_error,     sizeof(tune_beep_error)     / sizeof(Note)},
    {tune_startup,        sizeof(tune_startup)        / sizeof(Note)},
};


/* ═══════════════════════════════════════════════════════════════════════════
 * STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DEV_BOOT_WAITING,   /* I2C off, counting backoff timer */
    DEV_ENUM_READY,     /* I2C on at 0x7F, waiting for Conductor */
    DEV_ASSIGNING,      /* got new address, switch pending (done in main loop) */
    DEV_ASSIGNED,       /* normal operation at assigned address */
} DeviceState;

typedef enum { PLAY_IDLE, PLAY_NOTE, PLAY_TUNE } PlayState;

static volatile DeviceState dev_state  = DEV_BOOT_WAITING;
static volatile PlayState   play_state = PLAY_IDLE;
static volatile uint32_t    ms_tick    = 0;
static volatile uint8_t     new_addr   = 0;  /* set in ISR, used in main loop */

/* Playback */
static volatile uint32_t    note_start_ms = 0;
static volatile uint32_t    note_dur_ms   = 0;
static volatile const Note *tune_notes    = 0;
static volatile uint8_t     tune_len      = 0;
static volatile uint8_t     tune_idx      = 0;
static volatile uint32_t    tune_note_start_ms = 0;

/* I2C receive */
#define RX_BUF_SIZE 8
static volatile uint8_t rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_len    = 0;
static volatile uint8_t cmd_ready = 0;

/* I2C transmit (8 UID + 1 type + 1 CRC = 10 bytes) */
#define TX_BUF_SIZE 10
static volatile uint8_t tx_buf[TX_BUF_SIZE];
static volatile uint8_t tx_len = 0;
static volatile uint8_t tx_idx = 0;


/* ═══════════════════════════════════════════════════════════════════════════
 * CRC8  (polynomial 0x07)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * BACKOFF TIMER  —  proper FNV-1a hash of 8 UID bytes
 *
 * Bug fix v3.1: previous version computed h = seed XOR 2166136261 as the
 * starting value. When seed == 2166136261 (the FNV offset basis), this
 * gave h = 0, nullifying the offset and causing chips from the same batch
 * to produce near-identical backoff times (10 ms apart → collision).
 *
 * Fix: calc_backoff() now starts with h = 2166136261 (the FNV-1a offset)
 * directly, without XOR. This is correct FNV-1a and spreads same-batch
 * chips reliably across the full 300–2799 ms range.
 *
 * Re-backoff on collision uses ms_tick as seed and a shorter range (50–549ms)
 * so the Conductor doesn't have to wait long for retry.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t backoff_ms;
static uint32_t enum_ready_start_ms = 0;

/* Standard FNV-1a hash over 8 UID bytes starting from given h value */
static uint32_t fnv_hash(uint32_t h)
{
    volatile uint8_t *uid = UID_ADDR;
    for (uint8_t i = 0; i < UID_LEN; i++) {
        h ^= uid[i];
        h *= 16777619UL;
    }
    return h;
}

/* Initial backoff: proper FNV-1a, range 300–2799 ms */
static void calc_backoff(void)
{
    backoff_ms = (fnv_hash(2166136261UL) % 2500) + 300;
}

/* Re-backoff after collision: seed with ms_tick, short range 50–549 ms */
static uint32_t calc_rebackoff_ms(void)
{
    return ms_tick + (fnv_hash(ms_tick) % 500) + 50;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * BUILD UID RESPONSE  —  10 bytes: [UID 8 bytes] + [MODULE_TYPE] + [CRC8]
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_uid_response(void)
{
    volatile uint8_t *uid = UID_ADDR;
    for (uint8_t i = 0; i < UID_LEN; i++)
        tx_buf[i] = uid[i];
    tx_buf[8] = MODULE_TYPE;
    tx_buf[9] = crc8((const uint8_t*)tx_buf, 9);
    tx_len = 10;
    tx_idx = 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * PWM  —  TIM1 CH2 on PA1
 * ═══════════════════════════════════════════════════════════════════════════ */

static void pwm_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_TIM1;
    RCC->APB2PRSTR |=  RCC_APB2Periph_TIM1;
    RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;

    GPIOA->CFGLR &= ~(0xF << (1 * 4));
    GPIOA->CFGLR |=  (0x9 << (1 * 4));   /* AF push-pull, 10 MHz */

    TIM1->PSC    = 47;
    TIM1->ATRLR  = 999;
    TIM1->CH2CVR = 0;
    TIM1->CHCTLR1 &= ~(TIM_OC2M | TIM_OC2PE);
    TIM1->CHCTLR1 |=  (6 << 12) | TIM_OC2PE;
    TIM1->SWEVGR  = TIM_UG;
    TIM1->CTLR1  |= TIM_ARPE | TIM_CEN;
}

static void pwm_start(uint16_t freq, uint8_t vol)
{
    if (freq == 0) { TIM1->BDTR &= ~TIM_MOE; return; }
    if (vol > 100) vol = 100;
    uint32_t arr = (TIM_CLK_HZ / (uint32_t)freq) - 1;
    if (arr > 0xFFFF) arr = 0xFFFF;
    TIM1->ATRLR  = (uint16_t)arr;
    TIM1->CH2CVR = (uint16_t)(((arr + 1) * vol) / 200);
    TIM1->CCER  |= TIM_CC2E;
    TIM1->SWEVGR = TIM_UG;
    TIM1->BDTR  |= TIM_MOE;
}

static void pwm_stop(void) { TIM1->BDTR &= ~TIM_MOE; }


/* ═══════════════════════════════════════════════════════════════════════════
 * TIM2  —  1 ms tick
 * ═══════════════════════════════════════════════════════════════════════════ */

static void tim2_init(void)
{
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
    TIM2->PSC     = 47;
    TIM2->ATRLR   = 999;
    TIM2->DMAINTENR = TIM_IT_Update;
    TIM2->SWEVGR  = TIM_UG;
    TIM2->CTLR1  |= TIM_CEN;
    NVIC_EnableIRQ(TIM2_IRQn);
}

void TIM2_IRQHandler(void) __attribute__((interrupt));
void TIM2_IRQHandler(void) { TIM2->INTFR = 0; ms_tick++; }


/* ═══════════════════════════════════════════════════════════════════════════
 * I2C SLAVE  —  initialise at given address, or switch address at runtime
 * ═══════════════════════════════════════════════════════════════════════════ */

static void i2c_slave_init(uint8_t addr)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;

    GPIOC->CFGLR &= ~(0xF << (1 * 4)); GPIOC->CFGLR |= (0xF << (1 * 4));
    GPIOC->CFGLR &= ~(0xF << (2 * 4)); GPIOC->CFGLR |= (0xF << (2 * 4));

    I2C1->CTLR1 |=  I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    I2C1->CTLR2  = 48;
    I2C1->CKCFGR = 240;
    I2C1->OADDR1 = ((uint16_t)addr << 1);

    I2C1->CTLR2 |= I2C_CTLR2_ITEVTEN | I2C_CTLR2_ITBUFEN | I2C_CTLR2_ITERREN;
    I2C1->CTLR1 |= I2C_CTLR1_ACK | I2C_CTLR1_PE;

    NVIC_EnableIRQ(I2C1_EV_IRQn);
    NVIC_EnableIRQ(I2C1_ER_IRQn);
}

/* Switch to a new I2C address without full re-init */
static void i2c_switch_addr(uint8_t addr)
{
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    I2C1->OADDR1 = ((uint16_t)addr << 1);
    I2C1->CTLR1 |= I2C_CTLR1_ACK | I2C_CTLR1_PE;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * I2C EVENT ISR
 * Behaviour depends on dev_state:
 *   DEV_ENUM_READY  — TRA: send 10-byte UID response
 *                   — RXNE: buffer bytes
 *                   — STOPF: if [0x1D, addr] → flag address switch
 *   DEV_ASSIGNED    — TRA: send 1-byte status
 *                   — RXNE: buffer command bytes
 *                   — STOPF: signal main loop to process command
 * ═══════════════════════════════════════════════════════════════════════════ */

void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void)
{
    uint32_t star1 = I2C1->STAR1;
    uint32_t star2 = I2C1->STAR2;   /* reading STAR2 clears ADDR flag */

    /* ── Address matched ──────────────────────────────────────── */
    if (star1 & I2C_STAR1_ADDR)
    {
        rx_len = 0;
        I2C1->CTLR1 |= I2C_CTLR1_ACK;

        if (star2 & I2C_STAR2_TRA)
        {
            /* Master is reading from us */
            if (dev_state == DEV_ENUM_READY)
            {
                build_uid_response();
                I2C1->DATAR = tx_buf[tx_idx++];
            }
            else
            {
                /* Status byte: 1=playing, 0=idle */
                tx_buf[0] = (play_state != PLAY_IDLE) ? 0x01 : 0x00;
                tx_len = 1; tx_idx = 1;
                I2C1->DATAR = tx_buf[0];
            }
        }
        return;
    }

    /* ── Byte received ────────────────────────────────────────── */
    if (star1 & I2C_STAR1_RXNE)
    {
        uint8_t b = (uint8_t)I2C1->DATAR;
        if (rx_len < RX_BUF_SIZE) rx_buf[rx_len++] = b;
        return;
    }

    /* ── Transmit buffer empty ────────────────────────────────── */
    if (star1 & I2C_STAR1_TXE)
    {
        I2C1->DATAR = (tx_idx < tx_len) ? tx_buf[tx_idx++] : 0x00;
        return;
    }

    /* ── Stop condition ───────────────────────────────────────── */
    if (star1 & I2C_STAR1_STOPF)
    {
        I2C1->CTLR1 |= I2C_CTLR1_PE;   /* clear STOPF */

        if (dev_state == DEV_ENUM_READY)
        {
            /* Check for ASSIGN command: [0x1D, new_address] */
            if (rx_len == 2 && rx_buf[0] == REG_ASSIGN_ADDR)
            {
                new_addr  = rx_buf[1];
                dev_state = DEV_ASSIGNING;  /* switch address in main loop */
            }
        }
        else if (dev_state == DEV_ASSIGNED)
        {
            if (rx_len > 0) cmd_ready = 1;
        }

        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        return;
    }
}

void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void)
{
    I2C1->STAR1 &= ~(I2C_STAR1_BERR | I2C_STAR1_ARLO |
                     I2C_STAR1_AF   | I2C_STAR1_OVR);
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * COMMAND PROCESSOR
 * ═══════════════════════════════════════════════════════════════════════════ */

static void process_command(void)
{
    uint8_t cmd = rx_buf[0];

    if (cmd == CMD_STOP)
    {
        pwm_stop();
        play_state = PLAY_IDLE;
    }
    else if (cmd == CMD_PLAY_NOTE && rx_len >= 5)
    {
        uint16_t freq = ((uint16_t)rx_buf[1] << 8) | rx_buf[2];
        uint8_t  dur  = rx_buf[3];
        uint8_t  vol  = rx_buf[4];
        pwm_stop();
        note_dur_ms   = (dur == 0) ? 0 : (uint32_t)dur * 100;
        note_start_ms = ms_tick;
        play_state    = PLAY_NOTE;
        pwm_start(freq, vol);
    }
    else if (cmd == CMD_PLAY_TUNE && rx_len >= 2)
    {
        uint8_t id = rx_buf[1];
        if (id == 0 || id > TUNE_COUNT) return;
        pwm_stop();
        tune_notes         = tune_table[id].notes;
        tune_len           = tune_table[id].count;
        tune_idx           = 0;
        tune_note_start_ms = ms_tick;
        play_state         = PLAY_TUNE;
        pwm_start(tune_notes[0].freq, 100);
    }
    else if (cmd == CMD_ENTER_BOOTLOADER)
    {
        /* OTA update requested: arm the handoff magic and warm-reset into the
         * bootloader (0x7E). NVIC_SystemReset() does not clear SRAM. */
        BL_MAGIC_CELL = BL_MAGIC_ENTER;
        NVIC_SystemReset();
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * PLAYBACK UPDATER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_playback(void)
{
    uint32_t now = ms_tick;

    if (play_state == PLAY_NOTE)
    {
        if (note_dur_ms > 0 && (now - note_start_ms) >= note_dur_ms)
        {
            pwm_stop();
            play_state = PLAY_IDLE;
        }
    }
    else if (play_state == PLAY_TUNE)
    {
        if ((now - tune_note_start_ms) >= tune_notes[tune_idx].dur_ms)
        {
            tune_idx++;
            if (tune_idx >= tune_len)
            {
                pwm_stop();
                play_state = PLAY_IDLE;
            }
            else
            {
                tune_note_start_ms = now;
                pwm_start(tune_notes[tune_idx].freq, 100);
            }
        }
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    SystemInit();
    pwm_init();
    tim2_init();

    /* Calculate this module's unique backoff time from its hardware UID */
    calc_backoff();

    __enable_irq();

    /* Startup chime plays during backoff — confirms board is alive.
     * I2C is off during this time so the chime won't interfere. */
    rx_buf[0] = CMD_PLAY_TUNE; rx_buf[1] = 5;
    rx_len = 2;
    process_command();

    while (1)
    {
        uint32_t now = ms_tick;

        /* ── State transitions ─────────────────────────────────── */

        if (dev_state == DEV_BOOT_WAITING && now >= backoff_ms)
        {
            /* Backoff complete — join the bus at staging address */
            enum_ready_start_ms = now;
            i2c_slave_init(ENUM_ADDR);
            dev_state = DEV_ENUM_READY;
        }

        /* Safety net: if not assigned within 200 ms, a collision likely
         * occurred. Re-backoff with short range (50–549 ms) so the
         * Conductor finds the module quickly on the next attempt. */
        if (dev_state == DEV_ENUM_READY && (now - enum_ready_start_ms) > 200)
        {
            I2C1->CTLR1 &= ~I2C_CTLR1_PE;
            backoff_ms = calc_rebackoff_ms();
            dev_state  = DEV_BOOT_WAITING;
        }

        if (dev_state == DEV_ASSIGNING)
        {
            /* Switch to Conductor-assigned address */
            i2c_switch_addr(new_addr);
            dev_state = DEV_ASSIGNED;
        }

        /* ── Normal operation ──────────────────────────────────── */

        if (dev_state == DEV_ASSIGNED)
        {
            if (cmd_ready)
            {
                cmd_ready = 0;
                process_command();
            }
            update_playback();
        }
        else
        {
            /* During BOOT_WAITING / ENUM_READY: still update playback
             * so the startup chime plays correctly */
            update_playback();
        }
    }
}
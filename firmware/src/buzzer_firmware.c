/*
 * buzzer_firmware.c
 * I2C-controlled buzzer firmware for the noknok.app Buzzer Board
 * MCU  : CH32V003J4M6 (SOP-8) | Stack: cnlohr/ch32fun
 *
 * I2C Protocol (address 0x45):
 *   STOP  : write 1 byte  → 0x00
 *   BEEP  : write 4 bytes → 0x01, freq_hi, freq_lo, dur_100ms
 *
 * Pins: PA1=TIM1_CH2 (buzzer), PC1=SDA, PC2=SCL
 */

#include "ch32fun.h"
#include <stdint.h>

#define I2C_ADDRESS  0x45
#define CMD_STOP     0x00
#define CMD_BEEP     0x01

/* I2C receive buffer — written by ISR, read by main */
static volatile uint8_t rx_buf[4];
static volatile uint8_t rx_idx    = 0;
static volatile uint8_t cmd_len   = 0;
static volatile uint8_t cmd_ready = 0;


/* ── Buzzer (TIM1 CH2 on PA1, default mapping) ─────────────────────────── */
static void pwm_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_TIM1;
    RCC->APB2PRSTR |=  RCC_APB2Periph_TIM1;
    RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;

    /* PA1 → AF push-pull, 10 MHz */
    GPIOA->CFGLR &= ~(0xF << (1 * 4));
    GPIOA->CFGLR |=  (0x9 << (1 * 4));

    TIM1->PSC    = 47;  /* 48 MHz / 48 = 1 MHz timer clock */
    TIM1->ATRLR  = (1000000UL / 2700) - 1;
    TIM1->CTLR1 |= TIM_ARPE;

    TIM1->CH2CVR   = TIM1->ATRLR / 2;
    TIM1->CHCTLR1 &= ~(TIM_OC2M | TIM_OC2PE);
    TIM1->CHCTLR1 |=  (6 << 12) | TIM_OC2PE;

    TIM1->CCER   |= TIM_CC2E;
    /* MOE off until first beep command */
    TIM1->SWEVGR  = TIM_UG;
    TIM1->CTLR1  |= TIM_CEN;
}

static void buzzer_on(uint16_t freq_hz)
{
    if (freq_hz < 100 || freq_hz > 20000) return;
    uint32_t arr = (1000000UL / (uint32_t)freq_hz) - 1;
    if (arr > 0xFFFF) arr = 0xFFFF;
    TIM1->ATRLR  = (uint16_t)arr;
    TIM1->CH2CVR = (uint16_t)(arr / 2);
    TIM1->SWEVGR = TIM_UG;
    TIM1->BDTR  |= TIM_MOE;
}

static void buzzer_off(void)
{
    TIM1->BDTR &= ~TIM_MOE;
}


/* ── I2C Slave ─────────────────────────────────────────────────────────── */
static void i2c_slave_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;

    /* PC1=SDA, PC2=SCL → AF open-drain, 50 MHz */
    GPIOC->CFGLR &= ~(0xF << (1 * 4));
    GPIOC->CFGLR |=  (0xF << (1 * 4));
    GPIOC->CFGLR &= ~(0xF << (2 * 4));
    GPIOC->CFGLR |=  (0xF << (2 * 4));

    I2C1->CTLR1 |=  I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    I2C1->CTLR2  = 48;
    I2C1->CKCFGR = 240;
    I2C1->OADDR1 = (I2C_ADDRESS << 1);

    I2C1->CTLR2 |= I2C_CTLR2_ITEVTEN | I2C_CTLR2_ITBUFEN | I2C_CTLR2_ITERREN;
    I2C1->CTLR1 |= I2C_CTLR1_ACK | I2C_CTLR1_PE;

    NVIC_EnableIRQ(I2C1_EV_IRQn);
    NVIC_EnableIRQ(I2C1_ER_IRQn);
}

void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void)
{
    uint32_t star1 = I2C1->STAR1;
    (void)I2C1->STAR2;

    if (star1 & I2C_STAR1_ADDR)
    {
        rx_idx = 0;
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
    }
    if (star1 & I2C_STAR1_RXNE)
    {
        uint8_t b = (uint8_t)I2C1->DATAR;
        if (rx_idx < 4) rx_buf[rx_idx++] = b;
    }
    if (star1 & I2C_STAR1_STOPF)
    {
        cmd_len   = rx_idx;
        cmd_ready = 1;
        rx_idx    = 0;
        I2C1->CTLR1 |= I2C_CTLR1_PE;
    }
}

void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void)
{
    I2C1->STAR1 &= ~(I2C_STAR1_BERR | I2C_STAR1_ARLO |
                     I2C_STAR1_AF   | I2C_STAR1_OVR);
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
}


/* ── Main ──────────────────────────────────────────────────────────────── */
int main(void)
{
    SystemInit();
    pwm_init();

    /* Startup confirmation beep — confirms PWM is wired correctly */
    buzzer_on(2700);
    Delay_Ms(150);
    buzzer_off();

    i2c_slave_init();

    while (1)
    {
        if (!cmd_ready) continue;

        cmd_ready = 0;
        uint8_t len = cmd_len;
        uint8_t cmd = rx_buf[0];

        if (cmd == CMD_STOP)
        {
            buzzer_off();
        }
        else if (cmd == CMD_BEEP && len == 4)
        {
            uint16_t freq_hz = ((uint16_t)rx_buf[1] << 8) | rx_buf[2];
            uint16_t dur_ms  = (uint16_t)rx_buf[3] * 100;

            buzzer_on(freq_hz);
            Delay_Ms(dur_ms);
            buzzer_off();
        }
    }
}

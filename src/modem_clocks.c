/*
 * Modem clock initialisation for ESP32-C5 802.11p (ITS-G5) operation.
 *
 * Must be called before esp_wifi_init() so the PHY I2C bus is clocked.
 * The register sequence is order-sensitive — see comments below.
 */

#include "soc/soc.h"
#include "modem_clocks.h"

/* ESP32-C5 modem register constants (from modem_lpcon_reg.h, modem_syscon_reg.h) */
#define MODEM_LPCON_BASE 0x600AF000
#define MODEM_SYSCON_BASE 0x600A9C00

/* MODEM_LPCON registers */
#define MODEM_LPCON_CLK_CONF (MODEM_LPCON_BASE + 0x18)
#define MODEM_LPCON_CLK_CONF_FORCE_ON (MODEM_LPCON_BASE + 0x1C)
#define MODEM_LPCON_CLK_CONF_POWER_ST (MODEM_LPCON_BASE + 0x20)

/* MODEM_LPCON CLK_CONF bits */
#define MODEM_LPCON_CLK_WIFIPWR_EN BIT(0)
#define MODEM_LPCON_CLK_COEX_EN BIT(1)
#define MODEM_LPCON_CLK_I2C_MST_EN BIT(2)

/* MODEM_SYSCON registers */
#define MODEM_SYSCON_CLK_CONF_POWER_ST (MODEM_SYSCON_BASE + 0xC)
#define MODEM_SYSCON_MODEM_RST_CONF (MODEM_SYSCON_BASE + 0x10)
#define MODEM_SYSCON_CLK_CONF1 (MODEM_SYSCON_BASE + 0x14)

/* MODEM_SYSCON CLK_CONF1 bits */
#define MODEM_SYSCON_CLK_WIFIBB_22M_EN BIT(0)
#define MODEM_SYSCON_CLK_WIFIBB_44M_EN BIT(2)
#define MODEM_SYSCON_CLK_WIFIMAC_EN BIT(9)
#define MODEM_SYSCON_CLK_WIFI_APB_EN BIT(10)
#define MODEM_SYSCON_CLK_FE_40M_EN BIT(12)
#define MODEM_SYSCON_CLK_FE_80M_EN BIT(13)
#define MODEM_SYSCON_CLK_FE_160M_EN BIT(14)
#define MODEM_SYSCON_CLK_FE_APB_EN BIT(15)
#define MODEM_SYSCON_CLK_FE_PWDET_ADC_EN BIT(19)
#define MODEM_SYSCON_CLK_FE_ADC_EN BIT(20)
#define MODEM_SYSCON_CLK_FE_DAC_EN BIT(21)

/* MODEM_SYSCON MODEM_RST_CONF bits */
#define MODEM_SYSCON_RST_WIFIBB BIT(8)
#define MODEM_SYSCON_RST_WIFIMAC BIT(9)
#define MODEM_SYSCON_RST_FE BIT(14)

void modem_clocks_init(void)
{
    /* 1. MODEM_SYSCON CLK_CONF_POWER_ST: set clock state mappings.
     *    Value 6 = "force on", 4 = "on when modem active" */
    REG_WRITE(MODEM_SYSCON_CLK_CONF_POWER_ST,
              (6u << 28) | (4u << 24) | (6u << 20) | (4u << 16) | (6u << 12) | (4u << 8));

    /* 2. MODEM_LPCON CLK_CONF_POWER_ST: LP clock state mappings */
    REG_WRITE(MODEM_LPCON_CLK_CONF_POWER_ST,
              (6u << 28) | (6u << 24) | (6u << 20) | (6u << 16));

    /* 3. Enable I2C master, coex, and WiFi power clocks in MODEM_LPCON */
    REG_SET_BIT(MODEM_LPCON_CLK_CONF,
                MODEM_LPCON_CLK_I2C_MST_EN | MODEM_LPCON_CLK_COEX_EN | MODEM_LPCON_CLK_WIFIPWR_EN);

    /* 4. Enable WiFi baseband/MAC clocks in MODEM_SYSCON CLK_CONF1 */
    REG_SET_BIT(MODEM_SYSCON_CLK_CONF1,
                MODEM_SYSCON_CLK_WIFIBB_22M_EN |
                    MODEM_SYSCON_CLK_WIFIBB_44M_EN |
                    MODEM_SYSCON_CLK_WIFIMAC_EN |
                    MODEM_SYSCON_CLK_WIFI_APB_EN |
                    MODEM_SYSCON_CLK_FE_40M_EN |
                    MODEM_SYSCON_CLK_FE_80M_EN);

    /* 5. Force extra low-8 bits in CLK_CONF1 (mirrors esp_phy OR with 0x1fb) */
    REG_WRITE(MODEM_SYSCON_CLK_CONF1, REG_READ(MODEM_SYSCON_CLK_CONF1) | 0x1fb);

    /* 6. Enable frontend clocks in CLK_CONF1 */
    REG_SET_BIT(MODEM_SYSCON_CLK_CONF1,
                MODEM_SYSCON_CLK_FE_APB_EN |
                    MODEM_SYSCON_CLK_FE_160M_EN |
                    MODEM_SYSCON_CLK_FE_DAC_EN |
                    MODEM_SYSCON_CLK_FE_PWDET_ADC_EN |
                    MODEM_SYSCON_CLK_FE_ADC_EN);

    /* 7. Pulse modem reset: RF frontend, WiFi baseband, WiFi MAC.
     *    On PMU chips, system reset leaves modem domain powered, so
     *    stale state must be cleared explicitly. */
    REG_SET_BIT(MODEM_SYSCON_MODEM_RST_CONF,
                MODEM_SYSCON_RST_FE | MODEM_SYSCON_RST_WIFIBB | MODEM_SYSCON_RST_WIFIMAC);
    REG_CLR_BIT(MODEM_SYSCON_MODEM_RST_CONF,
                MODEM_SYSCON_RST_FE | MODEM_SYSCON_RST_WIFIBB | MODEM_SYSCON_RST_WIFIMAC);
}

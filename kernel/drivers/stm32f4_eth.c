/*
 *      This file is part of frosted.
 *
 *      frosted is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License version 2, as
 *      published by the Free Software Foundation.
 *
 *
 *      frosted is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with frosted.  If not, see <http://www.gnu.org/licenses/>.
 *
 *      Authors:
 *
 */
 
#include <stdint.h>
#include "frosted.h"
#include "gpio.h"
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/syscfg.h>
#include <libopencm3/ethernet/mac.h>
#include <libopencm3/ethernet/phy.h>

#define dbg(...)


    /* For STM32F4DIS-BB Discover-Mo board 
       ETH_MDIO --------------> PA2
       ETH_MDC ---------------> PC1

       ETH_RMII_REF_CLK-------> PA1

       ETH_RMII_CRS_DV -------> PA7
       ETH_MII_RX_ER   -------> PB10
       ETH_RMII_RXD0   -------> PC4
       ETH_RMII_RXD1   -------> PC5
       ETH_RMII_TX_EN  -------> PB11
       ETH_RMII_TXD0   -------> PB12
       ETH_RMII_TXD1   -------> PB13

       ETH_nRST_PIN    -------> PE2
       */

/* XXX: Bad idea to hardcode pinmux here ... */
const struct gpio_addr gpio_eth[] = {
    {.base=GPIOA, .pin=GPIO2, .mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // MDIO
    {.base=GPIOC, .pin=GPIO1, .mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // MDC
    {.base=GPIOA, .pin=GPIO1, .mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // RMII REF CLK (MCO out)
    {.base=GPIOA, .pin=GPIO7, .mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII CRS DV
    {.base=GPIOB, .pin=GPIO10,.mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII RXER
    {.base=GPIOC, .pin=GPIO4, .mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII RXD0
    {.base=GPIOC, .pin=GPIO5, .mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII RXD1
    {.base=GPIOB, .pin=GPIO11,.mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // RMII TXEN
    {.base=GPIOB, .pin=GPIO12,.mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // RMII TXD0
    {.base=GPIOB, .pin=GPIO13,.mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // RMII TXD1
    {.base=GPIOE, .pin=GPIO2, .mode=GPIO_MODE_OUTPUT, .optype=GPIO_OTYPE_PP, .name=NULL},            // PHY RESET
};


#define PHY_PHYID1              0x02    /**< PHYS ID 1.                     */
#define PHY_PHYID2              0x03    /**< PHYS ID 2.                     */

/* Some known PHY-identifiers */
#define PHY_KSZ8021_ID    0x00221556
#define PHY_KS8721_ID     0x00221610
#define PHY_DP83848I_ID   0x20005C90
#define PHY_LAN8710A_ID   0x0007C0F1
#define PHY_DM9161_ID     0x0181B8A0
#define PHY_AM79C875_ID   0x00225540
#define PHY_STE101P_ID    0x00061C50

#define BOARD_PHY_ID      PHY_LAN8710A_ID

static uint32_t eth_smi_get_phy_divider(void)
{
    uint32_t hclk = rcc_ahb_frequency;

#ifdef STM32F4
    /* CSR Clock between 150-168 MHz */ 
    if (hclk >= 150000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_102;    
#endif

    /* CSR Clock between 100-150 MHz */ 
    if (hclk >= 100000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_62;

    /* CSR Clock between 60-100 MHz */ 
    if (hclk >= 60000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_42;

    /* CSR Clock between 35-60 MHz */ 
    if (hclk >= 35000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_26;

    /* CSR Clock between 20-35 MHz */
    if (hclk >= 20000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_16;

    dbg("STM32_HCLK below minimum frequency for ETH operations (20MHz)\n");
    return 0;
}

static int8_t find_phy(void)
{
    uint32_t phy;

    for (phy = 0; phy < 31; phy++)
    {
        ETH_MACMIIDR = (phy << 6) | eth_smi_get_phy_divider();
        if ( (eth_smi_read(phy, PHY_PHYID1) == (BOARD_PHY_ID >> 16)) &&
            ((eth_smi_read(phy, PHY_PHYID2) & 0xFFF0) == (BOARD_PHY_ID & 0xFFF0)) )
            return (int8_t)phy;
    }
    /* PHY not detected */
    return -1;
}

static const uint8_t default_mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

void stm32f4_eth_init(void)
{
    int8_t phy_found;
    gpio_init(NULL, gpio_eth, sizeof(gpio_eth) / sizeof(struct gpio_addr));
    //gpio_set(GPIOC, GPIO0); // ???
    gpio_set(GPIOE,GPIO2); /* Clear RESET pin */

#define BOARD_PHY_RMII
#define SYSCFG_PMC_MII_RMII_SEL         ((uint32_t)0x00800000) /*!<Ethernet PHY interface selection */

#if defined(BOARD_PHY_RMII)
    SYSCFG_PMC |= SYSCFG_PMC_MII_RMII_SEL;
#else
    SYSCFG_PMC &= ~SYSCFG_PMC_MII_RMII_SEL;
#endif

    /* Enable RCC clocks: Eth, GPIO */
    rcc_periph_clock_enable(RCC_ETHMAC);
    rcc_periph_clock_enable(RCC_ETHMACTX);
    rcc_periph_clock_enable(RCC_ETHMACRX);

    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_GPIOE);

    rcc_osc_on(RCC_PLL);
    rcc_wait_for_osc_ready(RCC_PLL);
    
    rcc_periph_reset_pulse(RST_ETHMAC);

    eth_init(0, ETH_CLK_150_168MHZ); /* does a phy_reset */
    eth_set_mac((uint8_t*)default_mac);
    phy_found = find_phy();
}


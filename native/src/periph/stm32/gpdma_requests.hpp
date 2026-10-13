#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace popcorn::periph::stm32
{
/**
 * @brief One hardware request of a DMA controller: its signal name and the value of the
 *        channel's REQSEL field, GPDMA_CxTR2 or LPDMA_CxTR2.
 */
struct gpdma_request
{
  /** @brief Signal name as RM0456 spells it, such as `usart1_tx_dma`. */
  std::string_view signal;
  /** @brief Request number. */
  std::uint8_t request{};
};

/**
 * @brief GPDMA1 requests of the STM32U5 family, RM0456 section 17.3.3, Table 137 "Programmed
 *        GPDMA1 request". Request 86 is `dcmi_dma` or `pssi_dma`, both listed.
 */
inline constexpr std::array<gpdma_request, 127> gpdma_requests{
    {
     {"adc1_dma", 0},          {"adc4_dma", 1},           {"dac1_ch1_dma", 2},
     {"dac1_ch2_dma", 3},      {"tim6_upd_dma", 4},       {"tim7_upd_dma", 5},
     {"spi1_rx_dma", 6},       {"spi1_tx_dma", 7},        {"spi2_rx_dma", 8},
     {"spi2_tx_dma", 9},       {"spi3_rx_dma", 10},       {"spi3_tx_dma", 11},
     {"i2c1_rx_dma", 12},      {"i2c1_tx_dma", 13},       {"i2c1_evc_dma", 14},
     {"i2c2_rx_dma", 15},      {"i2c2_tx_dma", 16},       {"i2c2_evc_dma", 17},
     {"i2c3_rx_dma", 18},      {"i2c3_tx_dma", 19},       {"i2c3_evc_dma", 20},
     {"i2c4_rx_dma", 21},      {"i2c4_tx_dma", 22},       {"i2c4_evc_dma", 23},
     {"usart1_rx_dma", 24},    {"usart1_tx_dma", 25},     {"usart2_rx_dma", 26},
     {"usart2_tx_dma", 27},    {"usart3_rx_dma", 28},     {"usart3_tx_dma", 29},
     {"uart4_rx_dma", 30},     {"uart4_tx_dma", 31},      {"uart5_rx_dma", 32},
     {"uart5_tx_dma", 33},     {"lpuart1_rx_dma", 34},    {"lpuart1_tx_dma", 35},
     {"sai1_a_dma", 36},       {"sai1_b_dma", 37},        {"sai2_a_dma", 38},
     {"sai2_b_dma", 39},       {"octospi1_dma", 40},      {"octospi2_dma", 41},
     {"tim1_cc1_dma", 42},     {"tim1_cc2_dma", 43},      {"tim1_cc3_dma", 44},
     {"tim1_cc4_dma", 45},     {"tim1_upd_dma", 46},      {"tim1_trg_dma", 47},
     {"tim1_com_dma", 48},     {"tim8_cc1_dma", 49},      {"tim8_cc2_dma", 50},
     {"tim8_cc3_dma", 51},     {"tim8_cc4_dma", 52},      {"tim8_upd_dma", 53},
     {"tim8_trg_dma", 54},     {"tim8_com_dma", 55},      {"tim2_cc1_dma", 56},
     {"tim2_cc2_dma", 57},     {"tim2_cc3_dma", 58},      {"tim2_cc4_dma", 59},
     {"tim2_upd_dma", 60},     {"tim3_cc1_dma", 61},      {"tim3_cc2_dma", 62},
     {"tim3_cc3_dma", 63},     {"tim3_cc4_dma", 64},      {"tim3_upd_dma", 65},
     {"tim3_trg_dma", 66},     {"tim4_cc1_dma", 67},      {"tim4_cc2_dma", 68},
     {"tim4_cc3_dma", 69},     {"tim4_cc4_dma", 70},      {"tim4_upd_dma", 71},
     {"tim5_cc1_dma", 72},     {"tim5_cc2_dma", 73},      {"tim5_cc3_dma", 74},
     {"tim5_cc4_dma", 75},     {"tim5_upd_dma", 76},      {"tim5_trg_dma", 77},
     {"tim15_cc1_dma", 78},    {"tim15_upd_dma", 79},     {"tim15_trg_dma", 80},
     {"tim15_com_dma", 81},    {"tim16_cc1_dma", 82},     {"tim16_upd_dma", 83},
     {"tim17_cc1_dma", 84},    {"tim17_upd_dma", 85},     {"dcmi_dma", 86},
     {"pssi_dma", 86},         {"aes_in_dma", 87},        {"aes_out_dma", 88},
     {"hash_in_dma", 89},      {"ucpd1_tx_dma", 90},      {"ucpd1_rx_dma", 91},
     {"mdf1_flt0_dma", 92},    {"mdf1_flt1_dma", 93},     {"mdf1_flt2_dma", 94},
     {"mdf1_flt3_dma", 95},    {"mdf1_flt4_dma", 96},     {"mdf1_flt5_dma", 97},
     {"adf1_flt0_dma", 98},    {"fmac_read_dma", 99},     {"fmac_write_dma", 100},
     {"cordic_read_dma", 101}, {"cordic_write_dma", 102}, {"saes_in_dma", 103},
     {"saes_out_dma", 104},    {"lptim1_ic1_dma", 105},   {"lptim1_ic2_dma", 106},
     {"lptim1_ue_dma", 107},   {"lptim2_ic1_dma", 108},   {"lptim2_ic2_dma", 109},
     {"lptim2_ue_dma", 110},   {"lptim3_ic1_dma", 111},   {"lptim3_ic2_dma", 112},
     {"lptim3_ue_dma", 113},   {"hspi1_dma", 114},        {"i2c5_rx_dma", 115},
     {"i2c5_tx_dma", 116},     {"i2c5_evc_dma", 117},     {"i2c6_rx_dma", 118},
     {"i2c6_tx_dma", 119},     {"i2c6_evc_dma", 120},     {"usart6_rx_dma", 121},
     {"usart6_tx_dma", 122},   {"adc2_dma", 123},         {"jpeg_rx_dma", 124},
     {"jpeg_tx_dma", 125},
     }
};

/**
 * @brief LPDMA1 requests of the STM32U5 family, RM0456 section 18.3.3, Table 147 "Programmed
 *        LPDMA1 request".
 */
inline constexpr std::array<gpdma_request, 17> lpdma_requests{
    {
     {"lpuart1_rx_dma", 0},
     {"lpuart1_tx_dma", 1},
     {"spi3_rx_dma", 2},
     {"spi3_tx_dma", 3},
     {"i2c3_rx_dma", 4},
     {"i2c3_tx_dma", 5},
     {"i2c3_evc_dma", 6},
     {"adc4_dma", 7},
     {"dac1_ch1_dma", 8},
     {"dac1_ch2_dma", 9},
     {"adf1_flt0_dma", 10},
     {"lptim1_ic1_dma", 11},
     {"lptim1_ic2_dma", 12},
     {"lptim1_ue_dma", 13},
     {"lptim3_ic1_dma", 14},
     {"lptim3_ic2_dma", 15},
     {"lptim3_ue_dma", 16},
     }
};
}

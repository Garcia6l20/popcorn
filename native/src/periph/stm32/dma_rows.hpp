#pragma once

#include "bus_window.hpp"
#include "row_of.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief One hardware request of a classic DMA controller: its signal and its channel. */
struct dma_classic_request
{
  /** @brief Signal name, as dma_signal spells it, such as `usart1_tx_dma`. */
  std::string_view signal;
  /** @brief Channel the request is ORed into, 1 for channel 1 as the reference manual counts. */
  std::uint8_t channel;
  /** @brief Channel selection code that routes the request, 0 on a family without selection. */
  std::uint8_t code = 0;
};

/** @brief Differences between the classic DMA controllers the model rows serve. */
struct dma_classic_variant
{
  /** @brief Channels, numbered from 1. */
  std::uint8_t channels;
  /** @brief Hardware requests, each ORed into its channel. */
  std::span<dma_classic_request const> requests = {};
  /**
   * @brief APB windows behind a 32-bit AHB/APB bridge that ignores HSIZE: a byte or halfword
   *        write there is a word write with the item on every lane, RM0038 11.3.4.
   */
  std::span<bus_window const> apb_windows = {};
  /**
   * @brief Register of the per-channel selection fields `C<n>S`, empty on a family where every
   *        request of a channel is ORed into it. With one, a request reaches its channel only
   *        while the field holds the request's code.
   */
  std::string_view selection_register = {};
  /** @brief Whether a firmware byte or halfword write to a register warns, and raises under
   *         strict_peripherals. */
  bool word_only = false;
};

/**
 * @brief STM32L1 DMA1 requests, RM0038 11.3.7 Table 55. Signal names follow the requester models,
 *        `<instance>_<request>_dma` with the instance as the description names it.
 */
inline constexpr std::array<dma_classic_request, 33> l1_dma1_requests{
    {
     {"adc_dma", 1},       {"tim2_cc3_dma", 1}, {"tim4_cc1_dma", 1},  {"spi1_rx_dma", 2},
     {"usart3_tx_dma", 2}, {"tim2_upd_dma", 2}, {"tim3_cc3_dma", 2},  {"tim6_upd_dma", 2},
     {"dac_ch1_dma", 2},   {"spi1_tx_dma", 3},  {"usart3_rx_dma", 3}, {"tim3_cc4_dma", 3},
     {"tim3_upd_dma", 3},  {"tim7_upd_dma", 3}, {"dac_ch2_dma", 3},   {"spi2_rx_dma", 4},
     {"usart1_tx_dma", 4}, {"i2c2_tx_dma", 4},  {"tim4_cc2_dma", 4},  {"spi2_tx_dma", 5},
     {"usart1_rx_dma", 5}, {"i2c2_rx_dma", 5},  {"tim2_cc1_dma", 5},  {"tim4_cc3_dma", 5},
     {"usart2_rx_dma", 6}, {"i2c1_tx_dma", 6},  {"tim3_cc1_dma", 6},  {"tim3_trg_dma", 6},
     {"usart2_tx_dma", 7}, {"i2c1_rx_dma", 7},  {"tim2_cc2_dma", 7},  {"tim2_cc4_dma", 7},
     {"tim4_upd_dma", 7},
     }
};

/** @brief STM32L1 DMA2 requests, RM0038 11.3.7 Table 56, named as for DMA1. */
inline constexpr std::array<dma_classic_request, 15> l1_dma2_requests{
    {
     {"spi3_rx_dma", 1},
     {"uart5_tx_dma", 1},
     {"tim5_cc4_dma", 1},
     {"tim5_trg_dma", 1},
     {"spi3_tx_dma", 2},
     {"uart5_rx_dma", 2},
     {"tim5_cc3_dma", 2},
     {"tim5_upd_dma", 2},
     {"uart4_rx_dma", 3},
     {"aes_out_dma", 3},
     {"tim5_cc2_dma", 4},
     {"sdio_dma", 4},
     {"uart4_tx_dma", 5},
     {"tim5_cc1_dma", 5},
     {"aes_in_dma", 5},
     }
};

/** @brief STM32L1 DMA1, RM0038 11.1: 7 channels. */
inline constexpr dma_classic_variant l1_dma1{
    .channels    = 7,
    .requests    = l1_dma1_requests,
    .apb_windows = l1_apb_windows};
/** @brief STM32L1 DMA2, RM0038 11.1 and the 11.4 note: 5 channels, on Cat.3 to Cat.6. */
inline constexpr dma_classic_variant l1_dma2{
    .channels    = 5,
    .requests    = l1_dma2_requests,
    .apb_windows = l1_apb_windows};

/**
 * @brief STM32L47x/L48x DMA1 requests, RM0351 11.6.7 Table 46, by channel and `CxS` code. Signal
 *        names follow the requester models, as for the STM32L1. A signal the table lists on two
 *        channels, TIM16 and TIM17, has a row on each.
 */
inline constexpr std::array<dma_classic_request, 63> l4_dma1_requests{
    {
     {"adc1_dma", 1, 0},        {"tim2_cc3_dma", 1, 4},  {"tim17_cc1_dma", 1, 5},
     {"tim17_upd_dma", 1, 5},   {"tim4_cc1_dma", 1, 6},  {"adc2_dma", 2, 0},
     {"spi1_rx_dma", 2, 1},     {"usart3_tx_dma", 2, 2}, {"i2c3_tx_dma", 2, 3},
     {"tim2_upd_dma", 2, 4},    {"tim3_cc3_dma", 2, 5},  {"tim1_cc1_dma", 2, 7},
     {"adc3_dma", 3, 0},        {"spi1_tx_dma", 3, 1},   {"usart3_rx_dma", 3, 2},
     {"i2c3_rx_dma", 3, 3},     {"tim16_cc1_dma", 3, 4}, {"tim16_upd_dma", 3, 4},
     {"tim3_cc4_dma", 3, 5},    {"tim3_upd_dma", 3, 5},  {"tim6_upd_dma", 3, 6},
     {"dac1_ch1_dma", 3, 6},    {"tim1_cc2_dma", 3, 7},  {"dfsdm1_flt0_dma", 4, 0},
     {"spi2_rx_dma", 4, 1},     {"usart1_tx_dma", 4, 2}, {"i2c2_tx_dma", 4, 3},
     {"tim7_upd_dma", 4, 5},    {"dac1_ch2_dma", 4, 5},  {"tim4_cc2_dma", 4, 6},
     {"tim1_cc4_dma", 4, 7},    {"tim1_trg_dma", 4, 7},  {"tim1_com_dma", 4, 7},
     {"dfsdm1_flt1_dma", 5, 0}, {"spi2_tx_dma", 5, 1},   {"usart1_rx_dma", 5, 2},
     {"i2c2_rx_dma", 5, 3},     {"tim2_cc1_dma", 5, 4},  {"quadspi_dma", 5, 5},
     {"tim4_cc3_dma", 5, 6},    {"tim15_cc1_dma", 5, 7}, {"tim15_upd_dma", 5, 7},
     {"tim15_trg_dma", 5, 7},   {"tim15_com_dma", 5, 7}, {"dfsdm1_flt2_dma", 6, 0},
     {"sai2_a_dma", 6, 1},      {"usart2_rx_dma", 6, 2}, {"i2c1_tx_dma", 6, 3},
     {"tim16_cc1_dma", 6, 4},   {"tim16_upd_dma", 6, 4}, {"tim3_cc1_dma", 6, 5},
     {"tim3_trg_dma", 6, 5},    {"tim1_upd_dma", 6, 7},  {"dfsdm1_flt3_dma", 7, 0},
     {"sai2_b_dma", 7, 1},      {"usart2_tx_dma", 7, 2}, {"i2c1_rx_dma", 7, 3},
     {"tim2_cc2_dma", 7, 4},    {"tim2_cc4_dma", 7, 4},  {"tim17_cc1_dma", 7, 5},
     {"tim17_upd_dma", 7, 5},   {"tim4_upd_dma", 7, 6},  {"tim1_cc3_dma", 7, 7},
     }
};

/**
 * @brief STM32L47x/L48x DMA2 requests, RM0351 11.6.7 Table 47, as for DMA1. The I2C4, DCMI and
 *        HASH rows of the STM32L49x/L4Ax are left out. SAI1, AES and SDMMC1 have a row on each of
 *        their two channels.
 */
inline constexpr std::array<dma_classic_request, 49> l4_dma2_requests{
    {
     {"sai1_a_dma", 1, 1},    {"uart5_tx_dma", 1, 2},   {"spi3_rx_dma", 1, 3},
     {"swpmi1_rx_dma", 1, 4}, {"tim5_cc4_dma", 1, 5},   {"tim5_trg_dma", 1, 5},
     {"aes_in_dma", 1, 6},    {"tim8_cc3_dma", 1, 7},   {"tim8_upd_dma", 1, 7},
     {"sai1_b_dma", 2, 1},    {"uart5_rx_dma", 2, 2},   {"spi3_tx_dma", 2, 3},
     {"swpmi1_tx_dma", 2, 4}, {"tim5_cc3_dma", 2, 5},   {"tim5_upd_dma", 2, 5},
     {"aes_out_dma", 2, 6},   {"tim8_cc4_dma", 2, 7},   {"tim8_trg_dma", 2, 7},
     {"tim8_com_dma", 2, 7},  {"adc1_dma", 3, 0},       {"sai2_a_dma", 3, 1},
     {"uart4_tx_dma", 3, 2},  {"spi1_rx_dma", 3, 4},    {"aes_out_dma", 3, 6},
     {"adc2_dma", 4, 0},      {"sai2_b_dma", 4, 1},     {"tim6_upd_dma", 4, 3},
     {"dac1_ch1_dma", 4, 3},  {"spi1_tx_dma", 4, 4},    {"tim5_cc2_dma", 4, 5},
     {"sdmmc1_dma", 4, 7},    {"adc3_dma", 5, 0},       {"uart4_rx_dma", 5, 2},
     {"tim7_upd_dma", 5, 3},  {"dac1_ch2_dma", 5, 3},   {"tim5_cc1_dma", 5, 5},
     {"aes_in_dma", 5, 6},    {"sdmmc1_dma", 5, 7},     {"sai1_a_dma", 6, 1},
     {"usart1_tx_dma", 6, 2}, {"lpuart1_tx_dma", 6, 4}, {"i2c1_rx_dma", 6, 5},
     {"tim8_cc1_dma", 6, 7},  {"sai1_b_dma", 7, 1},     {"usart1_rx_dma", 7, 2},
     {"quadspi_dma", 7, 3},   {"lpuart1_rx_dma", 7, 4}, {"i2c1_tx_dma", 7, 5},
     {"tim8_cc2_dma", 7, 7},
     }
};

/** @brief STM32L47x/L48x DMA1 and DMA2, RM0351 section 11: 7 channels each, `DMA_CSELR`, word
 *         access only to the registers. */
inline constexpr dma_classic_variant l4_dma1{
    .channels           = 7,
    .requests           = l4_dma1_requests,
    .apb_windows        = l4_apb_windows,
    .selection_register = "CSELR",
    .word_only          = true};
/** @brief STM32L47x/L48x DMA2, as DMA1 with its own table. */
inline constexpr dma_classic_variant l4_dma2{
    .channels           = 7,
    .requests           = l4_dma2_requests,
    .apb_windows        = l4_apb_windows,
    .selection_register = "CSELR",
    .word_only          = true};

/**
 * @brief Factory of the classic channel DMA model, RM0038 section 11.
 * @param variant Channels and requests of the controller the row serves.
 */
[[nodiscard]] auto dma_classic_factory(dma_classic_variant variant) -> model_factory;

/** @brief Factory of the GPDMA1 model, RM0456 section 17. */
[[nodiscard]] auto gpdma_factory() -> model_factory;

/**
 * @brief Factory of the GPDMA1 and GPDMA2 model of the STM32H5, RM0481 section 16: 8 channels,
 *        2D addressing on 6 and 7, requests from Table 142, frozen by `DBG_GPDMA<n>_<ch>_STOP`.
 */
[[nodiscard]] auto gpdma_h5_factory() -> model_factory;

/** @brief Factory of the LPDMA1 model, the GPDMA engine with 4 channels, RM0456 section 18. */
[[nodiscard]] auto lpdma_factory() -> model_factory;

/** @brief The classic DMA model rows. */
[[nodiscard]] auto dma_classic_rows() -> std::span<builtin const>;

/** @brief The GPDMA and LPDMA model rows. */
[[nodiscard]] auto gpdma_rows() -> std::span<builtin const>;
}

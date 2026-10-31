#pragma once

#include "flash_unit.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Prefixes of the per-pin field names of a GPIO port, the pin number appended. */
struct gpio_field_names
{
  /** @brief `GPIOx_MODER` mode field. */
  std::string_view mode = "MODE";
  /** @brief `GPIOx_OTYPER` output type field. */
  std::string_view type = "OT";
  /** @brief `GPIOx_OSPEEDR` speed field. */
  std::string_view speed = "OSPEED";
  /** @brief `GPIOx_PUPDR` pull field. */
  std::string_view pull = "PUPD";
  /** @brief `GPIOx_IDR` input field. */
  std::string_view input = "ID";
  /** @brief `GPIOx_ODR` output field. */
  std::string_view output = "OD";
  /** @brief `GPIOx_AFRL` alternate function field of pins 0 to 7. */
  std::string_view alternate_low = "AFSEL";
  /** @brief `GPIOx_AFRH` alternate function field of pins 8 to 15. */
  std::string_view alternate_high = "AFSEL";
};

/**
 * @brief Differences between the GPIO ports of the families a model row serves.
 *        The model throws at attach when the description disagrees.
 */
struct gpio_variant
{
  /** @brief Port with fewer than 16 pins, 0 for none. */
  char short_port = 0;
  /** @brief Pins of that port. Its registers carry the per-pin fields of these pins only. */
  std::uint8_t short_port_pins = 16;
  /** @brief The port has `GPIOx_SECCFGR`, the TrustZone pin security register. */
  bool secure = true;
  /** @brief The port has `GPIOx_HSLVR`, the high-speed low-voltage register. */
  bool slew = true;
  /** @brief The port has `GPIOx_BRR`, the bit reset register. */
  bool brr = true;
  /** @brief Per-pin field names. */
  gpio_field_names names = {};
};

/** @brief STM32U5 ports with 16 pins, RM0456 13.4.10. */
inline constexpr gpio_variant u5_gpio{};
/** @brief STM32U575 and STM32U585 ports, port I has pins 0 to 7, read back on a NUCLEO-U575ZI-Q:
 *         `GPIOI_MODER` keeps 0x0000FFFF of 0xFFFFFFFF and `GPIOI_AFRH` reads 0, RM0456 13.4.1. */
inline constexpr gpio_variant u575_gpio{.short_port = 'I', .short_port_pins = 8};
/** @brief STM32H56x and H57x ports, port I has pins 0 to 11, read back on an STM32H573:
 *         `GPIOI_MODER` keeps 0x00FFFFFF of 0xFFFFFFFF and `GPIOI_AFRH` holds AFSEL8 to AFSEL11,
 *         as DS14121 Table 17 lists. */
inline constexpr gpio_variant h56x_gpio{.short_port = 'I', .short_port_pins = 12};

/** @brief STM32L1 per-pin field names, RM0038 7.4: MODERy, OSPEEDRy, PUPDRy, IDRy, ODRy, AFRLy
 *         and AFRHy. */
inline constexpr gpio_field_names l1_gpio_names{
    .mode           = "MODER",
    .speed          = "OSPEEDR",
    .pull           = "PUPDR",
    .input          = "IDR",
    .output         = "ODR",
    .alternate_low  = "AFRL",
    .alternate_high = "AFRH",
};
/** @brief STM32L1 Cat.1 to Cat.3 ports, `STM32L1_gpio_cat13`, RM0038 7.4: no `GPIOx_BRR`, which
 *         RM0038 7.4.11 gives to Cat.4, Cat.5 and Cat.6 only, no TrustZone and no HSLV. */
inline constexpr gpio_variant l1_gpio_cat13{
    .secure = false,
    .slew   = false,
    .brr    = false,
    .names  = l1_gpio_names};
/** @brief STM32L1 Cat.4 to Cat.6 ports, `STM32L1_gpio_cat46`, RM0038 7.4 and 7.4.11. */
inline constexpr gpio_variant l1_gpio_cat46{
    .secure = false,
    .slew   = false,
    .brr    = true,
    .names  = l1_gpio_names};

/** @brief NVIC line of the classic EXTI and the EXTI lines first to last that raise it. */
struct exti_line_group
{
  /** @brief First line. */
  std::uint8_t first;
  /** @brief Last line, included. */
  std::uint8_t last;
  /** @brief NVIC line, the IRQ number in the description. The CMSIS device header and the SVD
   *         name some of these lines differently, the number is the same in both. */
  std::uint16_t irq;
};

/** @brief Differences between the classic EXTI controllers the model rows serve. */
struct exti_classic_traits
{
  /** @brief Lines, the width of each register's line field. */
  std::uint8_t lines;
  /** @brief NVIC lines the EXTI raises. A line in no group sets its pending bit and raises
   *         nothing. */
  std::span<exti_line_group const> irqs;
};

/** @brief STM32L1, RM0038 Tables 49 to 51 by NVIC line: EXTI0 to EXTI4 on 6 to 10 one line each,
 *         EXTI9_5 on 23, EXTI15_10 on 40, line 17 RTC_Alarm on 41, line 19 the RTC tamper and
 *         timestamp line on 2, line 20 RTC_WKUP on 3, lines 21 and 22 the comparator wakeup line
 *         on 22, RM0038 20.5, line 18 USB_FS_WKUP on 42, RM0038 24.3.1. PVD and COMP_ACQ, lines
 *         16 and 23, belong to PWR and COMP in the description. */
inline constexpr auto l1_exti_irqs = std::to_array<exti_line_group>({
    {0,  0,  6 },
    {1,  1,  7 },
    {2,  2,  8 },
    {3,  3,  9 },
    {4,  4,  10},
    {5,  9,  23},
    {10, 15, 40},
    {17, 17, 41},
    {18, 18, 42},
    {19, 19, 2 },
    {20, 20, 3 },
    {21, 22, 22},
});
/** @brief STM32L1 Cat.1 and Cat.2, `STM32L1_exti_23`, RM0038 10.2: 23 lines. */
inline constexpr exti_classic_traits l1_exti_23{.lines = 23, .irqs = l1_exti_irqs};
/** @brief STM32L1 Cat.3 to Cat.6, RM0038 10.2: 24 lines. */
inline constexpr exti_classic_traits l1_exti_24{.lines = 24, .irqs = l1_exti_irqs};

/** @brief One hardware request of a classic DMA controller: its signal and its channel. */
struct dma_classic_request
{
  /** @brief Signal name, as dma_signal spells it, such as `usart1_tx_dma`. */
  std::string_view signal;
  /** @brief Channel the request is ORed into, 1 for channel 1 as the reference manual counts. */
  std::uint8_t channel;
};

/** @brief A bus address window, both ends included, and the clock domain of the bus. */
struct bus_window
{
  /** @brief First address. */
  std::uint32_t first;
  /** @brief Last address. */
  std::uint32_t last;
  /** @brief Clock domain of the bus, as the RCC model names it, such as `pclk1`. */
  std::string_view clock = {};
};

/**
 * @brief The window of @p windows holding @p address.
 * @return The window, or null when none holds it.
 */
[[nodiscard]] constexpr auto window_of(std::span<bus_window const> windows, std::uint32_t address)
    -> bus_window const*
{
  for(auto const& window : windows)
  {
    if(address >= window.first and address <= window.last)
    {
      return &window;
    }
  }
  return nullptr;
}

/** @brief Differences between the classic DMA controllers the model rows serve. */
struct dma_classic_traits
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
};

/** @brief STM32L1 APB1 and APB2, RM0038 Table 5. */
inline constexpr std::array<bus_window, 2> l1_apb_windows{
    {
     {.first = 0x4000'0000, .last = 0x4000'7FFF, .clock = "pclk1"},
     {.first = 0x4001'0000, .last = 0x4001'3FFF, .clock = "pclk2"},
     }
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
inline constexpr dma_classic_traits l1_dma1{
    .channels    = 7,
    .requests    = l1_dma1_requests,
    .apb_windows = l1_apb_windows};
/** @brief STM32L1 DMA2, RM0038 11.1 and the 11.4 note: 5 channels, on Cat.3 to Cat.6. */
inline constexpr dma_classic_traits l1_dma2{
    .channels    = 5,
    .requests    = l1_dma2_requests,
    .apb_windows = l1_apb_windows};

/** @brief One code of a `SYSCFG_EXTICRn` selector field. */
struct exti_port_code
{
  /** @brief Field value. */
  std::uint8_t code;
  /** @brief Port letter. */
  char port;
  /** @brief Lines that take the port at this code, bit n for line n. */
  std::uint16_t lines = 0xFFFF;
};

/** @brief Differences between the classic SYSCFG controllers the model rows serve, by register
 *         role. A role the family lacks is empty. */
struct syscfg_classic_traits
{
  /** @brief Selector codes. A code with no entry, or an entry without the line, selects no
   *         port. */
  std::span<exti_port_code const> ports;
  /** @brief Register of the USB D+ pull-up enable. */
  std::string_view usb_pull_up_register;
  /** @brief Field of the USB D+ pull-up enable. The other fields of its register are storage
   *         with the storage diagnostics. */
  std::string_view usb_pull_up_field;
  /** @brief Peer told of every change of the pull-up enable through usb_pull_up_listener. */
  std::string_view usb_peer;
  /** @brief Register of the memory remap, kept by a SYSCFGRST pulse. */
  std::string_view remap_register;
  /** @brief Registers served as storage the model does not model, with the storage
   *         diagnostics. */
  std::span<std::string_view const> storage_registers;
};

/** @brief RM0038 8.5.3 to 8.5.6, Cat.3 to Cat.6: PA to PE, PH for lines 0 to 2 only, PF and PG. */
inline constexpr auto l1_exti_ports = std::to_array<exti_port_code>({
    {0, 'A'},
    {1, 'B'},
    {2, 'C'},
    {3, 'D'},
    {4, 'E'},
    {5, 'H', 0x0007},
    {6, 'F'},
    {7, 'G'},
});
/** @brief STM32L1 storage registers of SYSCFG: MEMRMP. */
inline constexpr auto l1_syscfg_storage = std::to_array<std::string_view>({"MEMRMP"});
/** @brief STM32L1 Cat.3 to Cat.6, RM0038 section 8.5. */
inline constexpr syscfg_classic_traits l1_syscfg{
    .ports                = l1_exti_ports,
    .usb_pull_up_register = "PMC",
    .usb_pull_up_field    = "USB_PU",
    .usb_peer             = "USB",
    .remap_register       = "MEMRMP",
    .storage_registers    = l1_syscfg_storage};
/** @brief STM32L1 Cat.1 and Cat.2, `STM32L1_exti_23`, RM0038 8.5.3: codes 0110 and 0111, PF and
 *         PG, exist on Cat.3 to Cat.6 only. */
inline constexpr syscfg_classic_traits l1_syscfg_cat12{
    .ports                = std::span{l1_exti_ports}.first(6),
    .usb_pull_up_register = "PMC",
    .usb_pull_up_field    = "USB_PU",
    .usb_peer             = "USB",
    .remap_register       = "MEMRMP",
    .storage_registers    = l1_syscfg_storage};

/** @brief Differences between the STM32L1 FLASH controllers the model rows serve, by the
 *         `STM32L1_flash_<category>` version the category patches set. */
struct flash_l1_traits
{
  /** @brief Highest HCLK in kHz at 0 and 1 wait state in voltage ranges 1, 2 and 3, RM0038
   *         Table 13. */
  std::array<std::array<std::uint32_t, 2>, 3> max_khz;
  /** @brief Whether the part has two banks, FLASH_PECR.PARALLELBANK, RM0038 3.9.2. */
  bool dual_bank = false;
  /** @brief FLASH_WRPRx registers the category has, RM0038 3.9.9. */
  std::uint8_t wrp_registers = 1;
  /** @brief A byte or halfword store of 0 to the data EEPROM programs, RM0038 Table 21 note 7.
   *         Cat.1 programs only a non-zero byte or halfword, Table 20 note 7. */
  bool null_partial_writes = true;
  /** @brief FLASH_SR.OPTVERRUSR flags a USER option byte with a wrong complement, RM0038
   *         3.9.7: Cat.3 to Cat.6. */
  bool user_option_flag = true;
  /** @brief FLASH_OBR.SPRMOD turns the WRPx bits at 0 into PCROP read and write protection, and
   *         FLASH_SR.RDERR flags a data read of a PCROP sector, RM0038 3.7.4: the L151, L152
   *         and L162 Cat.2 and Cat.3 on DIE427. */
  bool pcrop = false;
};

/** @brief Table 13 on Cat.2 to Cat.6: Range 3 up to 4.2 MHz at 0 WS and 8 MHz at 1 WS. */
inline constexpr std::array<std::array<std::uint32_t, 2>, 3> l1_flash_wait_khz{
    {{16'000, 32'000}, {8'000, 16'000}, {4'200, 8'000}}
};
/** @brief STM32L1 Cat.1, `STM32L1_flash_cat1`: Range 3 up to 2.1 MHz at 0 WS and 4.2 MHz at
 *         1 WS, RM0038 Table 13. */
inline constexpr flash_l1_traits l1_flash_cat1{
    .max_khz             = {{{16'000, 32'000}, {8'000, 16'000}, {2'100, 4'200}}},
    .null_partial_writes = false,
    .user_option_flag    = false};
/** @brief STM32L1 Cat.2, `STM32L1_flash_cat2`. */
inline constexpr flash_l1_traits l1_flash_cat2{
    .max_khz          = l1_flash_wait_khz,
    .user_option_flag = false};
/** @brief STM32L151, L152 and L162 Cat.2, `STM32L1_flash_cat2_pcrop`: PCROP. */
inline constexpr flash_l1_traits l1_flash_cat2_pcrop{
    .max_khz          = l1_flash_wait_khz,
    .user_option_flag = false,
    .pcrop            = true};
/** @brief STM32L1 Cat.3, `STM32L1_flash_cat3`: FLASH_WRPR2. */
inline constexpr flash_l1_traits l1_flash_cat3{.max_khz = l1_flash_wait_khz, .wrp_registers = 2};
/** @brief STM32L151, L152 and L162 Cat.3 on DIE427, `STM32L1_flash_cat3_pcrop`: FLASH_WRPR2 and
 *         PCROP. */
inline constexpr flash_l1_traits l1_flash_cat3_pcrop{
    .max_khz       = l1_flash_wait_khz,
    .wrp_registers = 2,
    .pcrop         = true};
/** @brief STM32L1 Cat.4 and Cat.6, `STM32L1_flash_cat46`: two banks, FLASH_WRPR2 and
 *         FLASH_WRPR3. */
inline constexpr flash_l1_traits l1_flash_cat46{
    .max_khz       = l1_flash_wait_khz,
    .dual_bank     = true,
    .wrp_registers = 3};
/** @brief STM32L1 Cat.5, `STM32L1_flash_cat5`: two banks, FLASH_WRPR2 to FLASH_WRPR4. */
inline constexpr flash_l1_traits l1_flash_cat5{
    .max_khz       = l1_flash_wait_khz,
    .dual_bank     = true,
    .wrp_registers = 4};

/** @brief Differences between the FLASH controllers the model rows serve. */
struct flash_variant
{
  /** @brief Width of the SECWM, HDP and WRP page fields, RM0456 7.9.17. */
  std::uint8_t watermark_bits = 7;
  /** @brief Checks and encodes host option bytes for the unit and its program memory, and
   *         returns the boot address they select. Null when the row encodes no option bytes. */
  auto (*encode_options)(
      flash_variant const& variant,
      flash_program const& program,
      option_bytes const&  wanted) -> std::expected<address, option_refusal> = nullptr;
};

/** @brief The STM32U5 FLASH encoder, RM0456 7.4.1: the boot address of TZEN and the boot
 *         address options, @p program sized by its first region. */
[[nodiscard]] auto u5_encode_options(
    flash_variant const& variant,
    flash_program const& program,
    option_bytes const&  wanted) -> std::expected<address, option_refusal>;

/** @brief The STM32H5 FLASH encoder, RM0481 7.4.1, as u5_encode_options(). */
[[nodiscard]] auto h5_encode_options(
    flash_variant const& variant,
    flash_program const& program,
    option_bytes const&  wanted) -> std::expected<address, option_refusal>;

/**
 * @brief The STM32L1 FLASH encoder, RM0038 3.3.2 and Table 19: the flash base, or with
 *        `swap_bank` the bank 2 base, as the nBFB2 option byte at 0 selects on a part with two
 *        banks. Every other field refused when it is not its default, as `option_bytes.<field>`.
 * @param traits Category of the row.
 * @param program FLASH unit and its program memory regions.
 * @param wanted Option bytes the machine boots with.
 * @return The boot address, or the refusal.
 */
[[nodiscard]] auto l1_encode_options(
    flash_l1_traits const& traits,
    flash_program const&   program,
    option_bytes const&    wanted) -> std::expected<address, option_refusal>;

/** @brief l1_encode_options() for the category @p Traits, as a flash_variant encoder. */
template <flash_l1_traits const& Traits>
[[nodiscard]] auto l1_encode_options_of(
    flash_variant const&,
    flash_program const& program,
    option_bytes const&  wanted) -> std::expected<address, option_refusal>
{
  return l1_encode_options(Traits, program, wanted);
}

/** @brief STM32U5 parts with 7 bits. */
inline constexpr flash_variant flash_u5{.encode_options = u5_encode_options};
/** @brief STM32U535 and STM32U545, 5 bits. */
inline constexpr flash_variant flash_5bit{.watermark_bits = 5, .encode_options = u5_encode_options};
/** @brief STM32U59x, U5Ax, U5Fx and U5Gx, 8 bits. */
inline constexpr flash_variant flash_8bit{.watermark_bits = 8, .encode_options = u5_encode_options};
/** @brief STM32H5. */
inline constexpr flash_variant flash_h5{.encode_options = h5_encode_options};
/** @brief STM32L1 Cat.1. */
inline constexpr flash_variant flash_l1_cat1{.encode_options = l1_encode_options_of<l1_flash_cat1>};
/** @brief STM32L1 Cat.2. */
inline constexpr flash_variant flash_l1_cat2{.encode_options = l1_encode_options_of<l1_flash_cat2>};
/** @brief STM32L1 Cat.2 with PCROP. */
inline constexpr flash_variant flash_l1_cat2_pcrop{
    .encode_options = l1_encode_options_of<l1_flash_cat2_pcrop>};
/** @brief STM32L1 Cat.3. */
inline constexpr flash_variant flash_l1_cat3{.encode_options = l1_encode_options_of<l1_flash_cat3>};
/** @brief STM32L1 Cat.3 with PCROP. */
inline constexpr flash_variant flash_l1_cat3_pcrop{
    .encode_options = l1_encode_options_of<l1_flash_cat3_pcrop>};
/** @brief STM32L1 Cat.4 and Cat.6. */
inline constexpr flash_variant flash_l1_cat46{
    .encode_options = l1_encode_options_of<l1_flash_cat46>};
/** @brief STM32L1 Cat.5. */
inline constexpr flash_variant flash_l1_cat5{.encode_options = l1_encode_options_of<l1_flash_cat5>};

/** @brief Differences between the STM32H5 HASH processors the model rows serve, RM0481 40.3.
 *         A code outside the variant's set reads 0000 after a CR write and the hash runs SHA-1,
 *         as read back on an STM32H573. */
struct hash_variant
{
  /** @brief ALGO also keeps the SHA-3 and SHAKE codes 0100 to 1001. */
  bool sha3 = false;
};

/** @brief STM32H52x, H53x, H56x and H57x, SHA-1 and SHA-2 only, RM0481 Table 397: ALGO keeps
 *         0000, 0010, 0011 and 1100 to 1111. */
inline constexpr hash_variant hash_h5_sha2{};
/** @brief STM32H543 and H553, SHA-3 and SHAKE too, RM0481 Table 397. */
inline constexpr hash_variant hash_h5_sha3{.sha3 = true};

/** @brief Differences between the I2C peripherals the model rows serve. */
struct i2c_variant
{
  /** @brief The peripheral has the `I2C_AUTOCR` register and its trigger and DMA fields. */
  bool autocr = true;
};

/** @brief STM32U5 I2C, RM0456 65.7.14. */
inline constexpr i2c_variant u5_i2c{};
/** @brief STM32H5 I2C, RM0481 has no `I2C_AUTOCR`. */
inline constexpr i2c_variant h5_i2c{.autocr = false};

/** @brief Differences between the I2C peripherals of RM0038 section 26 the model rows serve. */
struct i2c_v1_variant
{
  /** @brief Buses whose clock is the clock of an instance in their window. */
  std::span<bus_window const> kernel_buses;
};

/** @brief STM32L1 I2C1 and I2C2 on APB1, RM0038 Table 5. */
inline constexpr i2c_v1_variant l1_i2c{.kernel_buses = l1_apb_windows};

/** @brief Register set of a USART IP version. */
enum class usart_registers : std::uint8_t
{
  /**
   * @brief `sci3`: ISR, ICR, RQR, RDR, TDR and PRESC, FIFO flags and thresholds, TEACK, REACK
   *        and BUSY, RM0456 66.8.
   */
  sci3,
  /**
   * @brief `sci2`: SR with `rc_w0` flags, one DR for both directions, SBK in CR1, the word length
   *        in CR1.M, BRR as DIV_Mantissa and DIV_Fraction, RM0038 27.6.
   */
  sci2,
};

/** @brief Differences between the USART, UART and LPUART peripherals the model rows serve. */
struct usart_variant
{
  /** @brief The peripheral has the `CR1_enabled` and `ISR_enabled` register views and no `CR1`
   *         and `ISR`. */
  bool mode_views = true;
  /** @brief The peripheral has the `FIFOEN` control bit, otherwise it holds one data word. */
  bool fifo = true;
  /** @brief Name of the IRQ line, empty for `<instance>_IRQn`. */
  std::string_view irq_name = {};
  /** @brief Register set of the IP version. */
  usart_registers registers = usart_registers::sci3;
  /**
   * @brief Buses whose clock is the kernel clock of an instance in their window. Empty: the RCC
   *        model names the kernel clock after the instance, such as `usart1`.
   */
  std::span<bus_window const> kernel_buses = {};
  /**
   * @brief High time after a break before the next frame, in half bits: 2 bits on `sci3`,
   *        RM0456 66.5.7, 1 bit on `sci2`, RM0038 27.3.2.
   */
  std::uint8_t break_stop_half_bits = 4;
  /**
   * @brief The instance has CTS and RTS flow control. Without it CTSE and RTSE are logged once
   *        and leave transmission and reception unchanged.
   */
  bool flow_control = true;
};

/** @brief STM32U5 USART and UART, RM0456 66.8, and every STM32H5 USART and UART, RM0481 56.8. */
inline constexpr usart_variant with_views_usart{};
/** @brief STM32U5 LPUART, RM0456 67.7, one `CR1` and one `ISR`. */
inline constexpr usart_variant u5_lpuart{.mode_views = false};
/** @brief STM32H5 LPUART, RM0481 57.7: the register views of the USART, and the CMSIS header
 *         names its line `LPUART1_IRQn` for the peripheral `LPUART`. */
inline constexpr usart_variant h5_lpuart{.irq_name = "LPUART1_IRQn"};
/** @brief STM32L1 USART and UART, RM0038 27.6, clocked by the APB bus of their address. */
inline constexpr usart_variant l1_usart{
    .mode_views           = false,
    .fifo                 = false,
    .registers            = usart_registers::sci2,
    .kernel_buses         = l1_apb_windows,
    .break_stop_half_bits = 2};
/** @brief STM32L1 UART4 and UART5: the L1 USART with no flow control, RM0038 Table 157. */
inline constexpr usart_variant l1_uart{
    .mode_views           = false,
    .fifo                 = false,
    .registers            = usart_registers::sci2,
    .kernel_buses         = l1_apb_windows,
    .break_stop_half_bits = 2,
    .flow_control         = false};

/** @brief Differences between the IWDG peripherals the model rows serve. */
struct iwdg_variant
{
  /** @brief The peripheral has the window: `WINR` and `SR.WVU`. Without it a refresh reloads at
   *         any counter value. */
  bool window = true;
  /** @brief The peripheral has the early wake-up interrupt: `EWCR`, `SR.EWU`, `SR.EWIF` and the
   *         `IWDG_IRQn` line. */
  bool early_wakeup = true;
  /** @brief Largest `PR` value with its own divider, 4 << `PR`. A larger value divides by
   *         4 << `prescaler_top`. */
  std::uint8_t prescaler_top = 8;
  /** @brief LSI cycles from a `PR`, `RLR`, `WINR` or `EWCR` write to the fall of its update
   *         flag. */
  std::uint8_t update_cycles = 6;
};

/** @brief STM32U5 and STM32H5 IWDG, RM0456 61.7.2 and Table 623, RM0481 49.7.2. */
inline constexpr iwdg_variant u5_iwdg{};
/** @brief STM32L1 IWDG, RM0038 21.4: KR, PR, RLR and SR with PVU and RVU only, PR 110 and 111
 *         both divide by 256, an update takes 5 LSI cycles. */
inline constexpr iwdg_variant l1_iwdg{
    .window        = false,
    .early_wakeup  = false,
    .prescaler_top = 6,
    .update_cycles = 5};

/** @brief Differences between the CRC peripherals the model rows serve. */
struct crc_variant
{
  /** @brief The unit has CRC_INIT, CRC_POL and the CR fields POLYSIZE, REV_IN and REV_OUT.
   *         Without them the unit computes CRC-32 over the polynomial 0x04C11DB7 from
   *         0xFFFFFFFF, with no reversal. */
  bool programmable = true;
  /** @brief A CRC_DR write narrower than 32 bits feeds the word its byte lanes repeat, such as
   *         0xB0B0B0B0 for the byte 0xB0, logs once and raises model_error under strict
   *         peripherals. Otherwise a right-aligned narrow write feeds its own width. */
  bool word_input = false;
  /** @brief A byte or half-word access to CRC_IDR is served as is, the CMSIS device header
   *         declaring it a byte. Otherwise a narrow access to it is a forbidden access. */
  bool byte_idr = false;
  /** @brief Manual section that states the access rules, named in the log records. */
  std::string_view access_rule = "RM0456 24.4";
};

/** @brief STM32U5 and STM32H5 CRC, RM0456 section 24. */
inline constexpr crc_variant u5_crc{};
/** @brief STM32L1 CRC, RM0038 section 4: DR, IDR and CR.RESET only, word accesses, the CMSIS
 *         `CRC_TypeDef` declaring IDR an `uint8_t`. */
inline constexpr crc_variant l1_crc{
    .programmable = false,
    .word_input   = true,
    .byte_idr     = true,
    .access_rule  = "RM0038 4.4"};

/** @brief Differences between the STM32L1 ADC categories the model rows serve, RM0038 12.3.3. */
struct adc_l1_variant
{
  /** @brief The unit has the two channel banks of ADC_CR2.ADC_CFG, ADC_SMPR0 for channels 30
   *         and 31 and the 28th rank SQ28. Without them it has one bank and 27 ranks. */
  bool banks = true;
  /** @brief External channels of bank A, bit n for ADC_INn, RM0038 Table 58. */
  std::uint32_t external = 0xFBFC'FFFF;
  /** @brief Bank B channels on pads of their own, bit n for ADC_INnb, RM0038 Table 41. The other
   *         bank B channels share the pad of their bank A channel. */
  std::uint32_t bank_b_only = 0x0000'1FCF;
  /** @brief Address of the factory VREFINT_CAL halfword. */
  std::uint32_t vrefint_cal = 0x1FF8'00F8;
  /** @brief Address of the factory TS_CAL1 halfword, the sensor code at 30 C. */
  std::uint32_t ts_cal1 = 0x1FF8'00FA;
};

/** @brief STM32L1 Cat.3 to Cat.6 ADC, DS8890, DS10262, DS10764 and DS10002. */
inline constexpr adc_l1_variant l1_adc{};
/** @brief STM32L1 Cat.1 and Cat.2 ADC, `STM32L1_adc_cat12`: one bank of ADC_IN0 to ADC_IN25,
 *         27 ranks, DS6876 and DS9515. */
inline constexpr adc_l1_variant l1_cat12_adc{
    .banks       = false,
    .external    = 0x03FC'FFFF,
    .bank_b_only = 0,
    .vrefint_cal = 0x1FF8'0078,
    .ts_cal1     = 0x1FF8'007A};

/** @brief Differences between the RTC and TAMP peripherals the model rows serve. */
struct rtc_variant
{
  /** @brief Name of the privilege register the security write handler binds. */
  std::string_view privilege_register = "PRIVCR";
};

/** @brief STM32U5 RTC and TAMP, RM0456 63.6.14 and 64.6.7. */
inline constexpr rtc_variant u5_rtc{};
/** @brief STM32H5 RTC and TAMP, RM0481 51.6.8 and 52.7.10. */
inline constexpr rtc_variant h5_rtc{.privilege_register = "PRIVCFGR"};

/** @brief Differences between the classic RTC peripherals the model rows serve, RM0038 20. */
struct rtc_classic_variant
{
  /** @brief Whether the RTC has subseconds and the shadow bypass: RTC_SSR, RTC_SHIFTR,
   *         RTC_TSSSR, RTC_ALRMASSR, RTC_ALRMBSSR, RTC_CR.BYPSHAD and RTC_ISR.SHPF. */
  bool subseconds = true;
  /** @brief Whether the RTC has smooth calibration: RTC_CALR and RTC_ISR.RECALPF. */
  bool smooth_calibration = true;
  /** @brief Whether the RTC has tamper inputs 2 and 3: TAMP2E, TAMP3E and their flags. */
  bool three_tampers = true;
  /** @brief Whether DBGMCU has DBG_RTC_STOP, so a held core can freeze the RTC. */
  bool debug_freeze = true;
  /** @brief Whether a brown-out resets the RTC domain as a power-on does, RM0038 6.1.2 and
   *         6.1.3 on a part with no VBAT supply. */
  bool brown_out_resets = true;
  /** @brief Whether ALRAWF and ALRBWF clear while their alarm is enabled and in init mode, and
   *         set up to 2 RTCCLK cycles after the alarm is disabled, RM0038 20.3.5 on Cat.1. Without
   *         it they read 1. */
  bool alarm_write_wait = false;
  /** @brief EXTI line of the alarm interrupt, RM0038 20.5. */
  std::uint8_t alarm_line = 17;
  /** @brief EXTI line of the wakeup interrupt, RM0038 20.5. */
  std::uint8_t wakeup_line = 20;
};

/** @brief STM32L1 Cat.2 to Cat.6 RTC, `rtc2_v2_5_Cube`, RM0038 section 20. */
inline constexpr rtc_classic_variant l1_rtc{};
/** @brief STM32L1 Cat.1 RTC, RM0038 20.2, 20.3.1, 20.3.5 and 30.16.4: no subseconds, no smooth
 *         calibration, one tamper input, no DBG_RTC_STOP and alarm write flags that follow the
 *         alarm enables. */
inline constexpr rtc_classic_variant l1_cat1_rtc{
    .subseconds         = false,
    .smooth_calibration = false,
    .three_tampers      = false,
    .debug_freeze       = false,
    .alarm_write_wait   = true};

/** @brief Differences between the SAI peripherals the model rows serve. */
struct sai_variant
{
  /** @brief Name of the block enable field of `xCR1`. */
  std::string_view enable = "SAIAEN";
};

/** @brief STM32U5 SAI, RM0456 69.6.2. */
inline constexpr sai_variant u5_sai{};
/** @brief STM32H5 SAI, RM0481 59.6.2. */
inline constexpr sai_variant h5_sai{.enable = "SAIEN"};

/** @brief The option register field value under which a trigger_entry holds. */
struct trigger_remap
{
  /** @brief Register of the field, such as `OR`. */
  std::string_view reg;
  /** @brief Field that selects between the inputs of one TS value, such as `ITR1_RMP`. */
  std::string_view field;
  /** @brief Field value that selects this entry's source. */
  std::uint32_t value;
};

/** @brief One internal trigger input of a timer's slave mode controller. */
struct trigger_entry
{
  /** @brief SMCR.TS value that selects the input. */
  std::uint32_t select;
  /** @brief Source whose `<source>_trgo` the input carries: a timer, such as `tim2`, the OC1
   *         output of a timer, such as `tim10_oc1`, which that timer drives from OC1 as
   *         `tim10_oc1_trgo`, or a source no model drives, such as `usbsof`. An OC1 source counts
   *         up only, its CR1 has no DIR and no CMS. */
  std::string_view source;
  /** @brief Instance whose slave mode controller has the input, empty for every timer. */
  std::string_view slave{};
  /** @brief Option register field value the input needs, empty when TS alone selects it. */
  std::optional<trigger_remap> remap{};
};

/** @brief A timer whose tim_etr_in is a pin signal, with no ETRSEL field. */
struct etr_pin
{
  /** @brief Instance, such as `TIM3`. */
  std::string_view instance;
  /** @brief Pin signal that carries ETR, such as `tim3_etr`. */
  std::string_view signal;
};

/** @brief An option register field that connects a timer input to its pin or to an internal
 *         source. */
struct input_remap
{
  /** @brief Instance that has the field. */
  std::string_view slave;
  /** @brief Input the field selects for, 0 for tim_ti1. */
  std::uint8_t channel;
  /** @brief Register of the field, such as `OR`. */
  std::string_view reg;
  /** @brief Field name, such as `TI1_RMP`. */
  std::string_view field;
  /** @brief Bit v set when field value v keeps the input on its pin. */
  std::uint32_t pin_values;
};

/** @brief Differences between the timer peripherals the model rows serve. */
struct tim_variant
{
  /** @brief Internal trigger inputs by TS value. The input a timer would take from itself is
   *         reserved, and TS values with no entry reach nothing. */
  std::span<trigger_entry const> triggers;
  /** @brief Instances whose CR2.MMS has 4 bits, bit 3 in field `MMS_1`. The other timers with an
   *         MMS field have 3 bits. */
  std::span<std::string_view const> wide_master;
  /** @brief Instances with no master mode, so no CR2.MMS field. Every other timer has one. */
  std::span<std::string_view const> no_master;
  /** @brief SMCR.SMS and SMCR.TS have their high bits in fields `SMS_1` and `TS_1`. Otherwise
   *         SMS and TS are 3 bits wide and neither field exists. */
  bool slave_high = true;
  /** @brief CR1 has `UIFREMAP`, which copies UIF into `CNT.UIFCPY` where CNT has that field.
   *         Otherwise neither field exists. */
  bool uif_remap = true;
  /** @brief Instances whose tim_etr_in is a pin signal, with no ETRSEL field. The other timers
   *         have ETR only through `AF1.ETRSEL`. */
  std::span<etr_pin const> etr_pins{};
  /** @brief When set, the instances that count up only: CR1 has no DIR and no CMS, and SMS 001
   *         to 011 are reserved. Every other instance of the variant has DIR and CMS. Unset, DIR
   *         and CMS are what the description gives. */
  std::optional<std::span<std::string_view const>> up_only{};
  /** @brief Option register fields that select a timer input's source. A value the entry's
   *         `pin_values` lacks gives an internal source. */
  std::span<input_remap const> input_remaps{};
};

/** @brief RM0456 54.6.2 and 55.5.2, RM0481 43.6.2 and 44.5.2: MMS[3:0] on TIM1, TIM8 and TIM2 to
 *         TIM5. RM0456 56.7.2, 57.4.2 and RM0481 45.4.2, 46.7.2, 47.7.2: MMS[2:0] on the others. */
inline constexpr auto wide_master_timers =
    std::to_array<std::string_view>({"TIM1", "TIM2", "TIM3", "TIM4", "TIM5", "TIM8"});

/** @brief TS 0 to 3 are ITR0 to ITR3, 8 to 10 are ITR4 to ITR6, RM0456 54.6.3 and 55.5.3 TIMx_SMCR.
 */
inline constexpr auto u5_triggers = std::to_array<trigger_entry>({
    {0,  "tim1" },
    {1,  "tim2" },
    {2,  "tim3" },
    {3,  "tim4" },
    {8,  "tim5" },
    {9,  "tim8" },
    {10, "tim15"},
});
/** @brief RM0456 56.8.2: TIM16 and TIM17 have no master mode. */
inline constexpr auto u5_no_master = std::to_array<std::string_view>({"TIM16", "TIM17"});
/** @brief STM32U5 timers. */
inline constexpr tim_variant u5_tim{u5_triggers, wide_master_timers, u5_no_master};

/**
 * @brief TS 0 to 3 are ITR0 to ITR3, 8 to 15 are ITR4 to ITR11 and 16 is ITR12, RM0481 43.6.3
 *        and 44.5.3 TIMx_SMCR. RM0481 Tables 456, 480, 502 and 514 connect the same source to the
 *        same ITR on TIM1, TIM2 to TIM5, TIM8, TIM12 and TIM15. ITR12 is USBSOF on TIM2 and TIM5
 *        and reserved on the other six. USBSOF has no model.
 */
inline constexpr auto h5_triggers = std::to_array<trigger_entry>({
    {0,  "tim1"     },
    {1,  "tim2"     },
    {2,  "tim3"     },
    {3,  "tim4"     },
    {8,  "tim5"     },
    {9,  "tim8"     },
    {10, "tim12"    },
    {11, "tim13_oc1"},
    {12, "tim14_oc1"},
    {13, "tim15"    },
    {14, "tim16_oc1"},
    {15, "tim17_oc1"},
    {16, "usbsof"   },
});
/** @brief RM0481 46.8.13: TIM13 and TIM14 have no CR2. RM0481 47.8.2: TIM16 and TIM17 have no
 *         master mode. */
inline constexpr auto h5_no_master =
    std::to_array<std::string_view>({"TIM13", "TIM14", "TIM16", "TIM17"});
/** @brief STM32H5 timers. */
inline constexpr tim_variant h5_tim{h5_triggers, wide_master_timers, h5_no_master};

/**
 * @brief RM0038 Table 80, TIMx internal trigger connection, on Cat.1 and Cat.2: ITR0 to ITR3 by
 *        TS 000 to 011 per slave timer. Cat.1 and Cat.2 have no TIM2_OR or TIM3_OR, RM0038
 *        17.4.19 and 17.4.20, so TIM2 ITR1 is TIM10 OC1 and TIM3 ITR2 is TIM11 OC1, the inputs
 *        those registers give at reset. Table 80 has no TIM5 row, so TIM5 has no entry.
 */
inline constexpr auto l1_triggers = std::to_array<trigger_entry>({
    {.select = 0, .source = "tim9",      .slave = "TIM2"},
    {.select = 1, .source = "tim10_oc1", .slave = "TIM2"},
    {.select = 2, .source = "tim3",      .slave = "TIM2"},
    {.select = 3, .source = "tim4",      .slave = "TIM2"},
    {.select = 0, .source = "tim9",      .slave = "TIM3"},
    {.select = 1, .source = "tim2",      .slave = "TIM3"},
    {.select = 2, .source = "tim11_oc1", .slave = "TIM3"},
    {.select = 3, .source = "tim4",      .slave = "TIM3"},
    {.select = 0, .source = "tim10_oc1", .slave = "TIM4"},
    {.select = 1, .source = "tim2",      .slave = "TIM4"},
    {.select = 2, .source = "tim3",      .slave = "TIM4"},
    {.select = 3, .source = "tim9",      .slave = "TIM4"},
    {.select = 0, .source = "tim2",      .slave = "TIM9"},
    {.select = 1, .source = "tim3",      .slave = "TIM9"},
    {.select = 2, .source = "tim10_oc1", .slave = "TIM9"},
    {.select = 3, .source = "tim11_oc1", .slave = "TIM9"},
});
/**
 * @brief RM0038 Table 80 on Cat.3 to Cat.6: TIM2_OR.ITR1_RMP selects TIM10 OC1 (0) or TIM5 TRGO
 *        (1) for TIM2 ITR1, TIM3_OR.ITR2_RMP selects TIM11 OC1 (0) or TIM5 TRGO (1) for TIM3 ITR2,
 *        RM0038 17.4.19 and 17.4.20.
 */
inline constexpr auto l1_cat3_triggers = std::to_array<trigger_entry>({
    {.select = 0, .source = "tim9", .slave = "TIM2"},
    {.select = 1,
     .source = "tim10_oc1",
     .slave  = "TIM2",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 0}},
    {.select = 1,
     .source = "tim5",
     .slave  = "TIM2",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 1}},
    {.select = 2, .source = "tim3", .slave = "TIM2"},
    {.select = 3, .source = "tim4", .slave = "TIM2"},
    {.select = 0, .source = "tim9", .slave = "TIM3"},
    {.select = 1, .source = "tim2", .slave = "TIM3"},
    {.select = 2,
     .source = "tim11_oc1",
     .slave  = "TIM3",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR2_RMP", .value = 0}},
    {.select = 2,
     .source = "tim5",
     .slave  = "TIM3",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR2_RMP", .value = 1}},
    {.select = 3, .source = "tim4", .slave = "TIM3"},
    {.select = 0, .source = "tim10_oc1", .slave = "TIM4"},
    {.select = 1, .source = "tim2", .slave = "TIM4"},
    {.select = 2, .source = "tim3", .slave = "TIM4"},
    {.select = 3, .source = "tim9", .slave = "TIM4"},
    {.select = 0, .source = "tim2", .slave = "TIM9"},
    {.select = 1,
     .source = "tim3",
     .slave  = "TIM9",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 0}},
    {.select = 1,
     .source = "ts_io",
     .slave  = "TIM9",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 1}},
    {.select = 2, .source = "tim10_oc1", .slave = "TIM9"},
    {.select = 3, .source = "tim11_oc1", .slave = "TIM9"},
});
/**
 * @brief RM0038 17.3.3 and 17.4.3: TIM2 to TIM5 take TIMx_ETR from their ETR pin. The datasheet
 *        pin tables name the TIM2 pad TIM2_CH1_ETR, at AF1 on PA0, PA5, PA15 and PE9, so the
 *        `tim2_ch1` alternate carries both TI1 and ETR.
 */
inline constexpr auto l1_etr_pins = std::to_array<etr_pin>({
    {.instance = "TIM2", .signal = "tim2_ch1"},
    {.instance = "TIM3", .signal = "tim3_etr"},
    {.instance = "TIM4", .signal = "tim4_etr"},
    {.instance = "TIM5", .signal = "tim5_etr"},
});
/**
 * @brief RM0038 18.2.1, 18.2.2, 18.4.1, 18.5.1 and 19.2: TIM6, TIM7, TIM10, TIM11 and the Cat.1 and
 *        Cat.2 TIM9 count up only.
 */
inline constexpr auto l1_up_only =
    std::to_array<std::string_view>({"TIM6", "TIM7", "TIM9", "TIM10", "TIM11"});
/** @brief RM0038 18.2.1 and 19.2: on Cat.3 and up TIM9 counts up, down and centre-aligned. */
inline constexpr auto l1_cat3_up_only =
    std::to_array<std::string_view>({"TIM6", "TIM7", "TIM10", "TIM11"});
/** @brief RM0038 18.5: TIM10 and TIM11 have no CR2. */
inline constexpr auto l1_no_master = std::to_array<std::string_view>({"TIM10", "TIM11"});
/**
 * @brief RM0038 18.4.14 TIM9_OR: TI1_RMP 01 connects the LSE to tim_ti1, 00, 10 and 11 the
 *        TIM9_CH1 pin. 18.5.12 TIM10_OR: TI1_RMP 00 the TIM10_CH1 pin, 01 to 11 the LSI, the LSE
 *        and the RTC wakeup interrupt. 18.5.13 TIM11_OR: TI1_RMP 00 and 11 the TIM11_CH1 pin, 01
 *        and 10 the MSI and HSE_RTC.
 */
inline constexpr auto l1_input_remaps = std::to_array<input_remap>({
    {.slave = "TIM9",  .channel = 0, .reg = "OR", .field = "TI1_RMP", .pin_values = 0b1101},
    {.slave = "TIM10", .channel = 0, .reg = "OR", .field = "TI1_RMP", .pin_values = 0b0001},
    {.slave = "TIM11", .channel = 0, .reg = "OR", .field = "TI1_RMP", .pin_values = 0b1001},
});
/**
 * @brief The Cat.1 and Cat.2 remaps, and RM0038 18.5.12 and 18.5.13 on Cat.3 to Cat.6: TI1_RMP_RI 1
 *        connects the routing interface to tim_ti1 of TIM10 and TIM11, whatever TI1_RMP.
 */
inline constexpr auto l1_cat3_input_remaps = std::to_array<input_remap>({
    {.slave = "TIM9",  .channel = 0, .reg = "OR", .field = "TI1_RMP",    .pin_values = 0b1101},
    {.slave = "TIM10", .channel = 0, .reg = "OR", .field = "TI1_RMP",    .pin_values = 0b0001},
    {.slave = "TIM10", .channel = 0, .reg = "OR", .field = "TI1_RMP_RI", .pin_values = 0b01  },
    {.slave = "TIM11", .channel = 0, .reg = "OR", .field = "TI1_RMP",    .pin_values = 0b1001},
    {.slave = "TIM11", .channel = 0, .reg = "OR", .field = "TI1_RMP_RI", .pin_values = 0b01  },
});
/**
 * @brief STM32L1 TIM2 to TIM4, TIM6, TIM7 and TIM9 to TIM11 of Cat.1 and Cat.2, RM0038 17.4, 18.4,
 *        18.5 and 19.4: CR2.MMS[2:0], SMCR.SMS[2:0] and TS[2:0], no UIFREMAP.
 */
inline constexpr tim_variant l1_tim{
    .triggers     = l1_triggers,
    .wide_master  = {},
    .no_master    = l1_no_master,
    .slave_high   = false,
    .uif_remap    = false,
    .etr_pins     = l1_etr_pins,
    .up_only      = l1_up_only,
    .input_remaps = l1_input_remaps};
/**
 * @brief STM32L1 TIM2 to TIM7 and TIM9 to TIM11 of Cat.3 to Cat.6, with the TIM2_OR, TIM3_OR,
 *        TIM9_OR, TIM10_OR and TIM11_OR remaps.
 */
inline constexpr tim_variant l1_cat3_tim{
    .triggers     = l1_cat3_triggers,
    .wide_master  = {},
    .no_master    = l1_no_master,
    .slave_high   = false,
    .uif_remap    = false,
    .etr_pins     = l1_etr_pins,
    .up_only      = l1_cat3_up_only,
    .input_remaps = l1_cat3_input_remaps};

/** @brief Differences between the SPI peripherals the model rows serve. */
struct spi_variant
{
  /** @brief Instances with an 8-byte FIFO, the others hold 16 bytes. */
  std::span<std::string_view const> limited;
  /** @brief The limited instances have the `CTSIZE` remaining-frames counter. */
  bool limited_counter = false;
  /** @brief The peripheral has the `SPI_AUTOCR` register and its trigger field. */
  bool autocr = true;
  /** @brief `SPI_CFG1` TXDMAEN and RXDMAEN stay writable while SPE = 1, the rest of the register
   * is write-protected then. Otherwise the whole register is. */
  bool dma_enables_writable = true;
};

inline constexpr auto u5_limited = std::to_array<std::string_view>({"SPI3"});
/** @brief STM32U5 SPI, RM0456 68.3: SPI3 has an 8-byte FIFO and no `CTSIZE`. RM0456 68.8.3:
 *         `SPI_CFG1` is write-protected while SPE = 1 except TXDMAEN and RXDMAEN. */
inline constexpr spi_variant u5_spi{u5_limited};

inline constexpr auto h5_limited = std::to_array<std::string_view>({"SPI4", "SPI5", "SPI6"});
/** @brief STM32H5 SPI, RM0481 Table 645: SPI4 to SPI6 have an 8-byte FIFO and keep TSIZE up to
 *         65535, so `CTSIZE` counts there. RM0481 section 58.11 has no `SPI_AUTOCR`. RM0481
 *         58.11.3: the whole `SPI_CFG1` is write-protected while SPE = 1. */
inline constexpr spi_variant h5_spi{
    .limited              = h5_limited,
    .limited_counter      = true,
    .autocr               = false,
    .dma_enables_writable = false};

/** @brief Differences between the classic SPI peripherals the model rows serve. */
struct spi_classic_variant
{
  /** @brief The peripheral has the TI frame format, `CR2.FRF`. Without it the bit is storage. */
  bool ti_mode;
  /** @brief Buses whose clock is the clock of an instance in their window. */
  std::span<bus_window const> kernel_buses;
};

/** @brief STM32L1 SPI of Cat.3 to Cat.6, RM0038 28.2.1: the TI mode. */
inline constexpr spi_classic_variant l1_spi_ti{.ti_mode = true, .kernel_buses = l1_apb_windows};
/** @brief STM32L1 SPI of Cat.1 and Cat.2, RM0038 28.2.1 and 28.5.2 FRF: no TI mode. */
inline constexpr spi_classic_variant l1_spi{.ti_mode = false, .kernel_buses = l1_apb_windows};

/** @brief A TZSC `<STEM>SEC` or TZIC `<STEM>IE` stem whose peripheral has another name. */
struct gtzc_rename
{
  /** @brief Stem of the field. */
  std::string_view stem;
  /** @brief Peripheral the stem names. */
  std::string_view peripheral;
};

/** @brief A peripheral that one TZSC or TZIC bit covers together with another. */
struct gtzc_companion
{
  /** @brief Peripheral the stem names, after the renames. */
  std::string_view primary;
  /** @brief Peripheral the same bit also covers. */
  std::string_view also;
};

/** @brief Differences between the GTZC TZSC and TZIC field stems of the families a model row
 *         serves: the stems that name no peripheral of the same name. */
struct gtzc_variant
{
  /** @brief Stems naming a peripheral of another name. */
  std::span<gtzc_rename const> renames;
  /** @brief Peripherals one bit covers with the peripheral it names. */
  std::span<gtzc_companion const> companions;
};

inline constexpr auto u5_gtzc_renames    = std::to_array<gtzc_rename>({
    {"USART4",       "UART4"   },
    {"ICACHE",       "ICache"  },
    {"ICACHE_REG",   "ICache"  },
    {"DCACHE_REG",   "DCACHE"  },
    {"OTGFS",        "OTG_FS"  },
    {"FSMC",         "FMC"     },
    {"FSMC_REG",     "FMC"     },
    {"OCTOSPI1_REG", "OCTOSPI1"},
    {"OCTOSPI2_REG", "OCTOSPI2"},
    {"FLASH_REG",    "FLASH"   },
});
inline constexpr auto u5_gtzc_companions = std::to_array<gtzc_companion>({
    {"DCMI",     "PSSI"      },
    {"ADC1",     "ADC12"     },
    {"FDCAN1",   "FDCAN1_RAM"},
    {"SDMMC1",   "DLYBSD1"   },
    {"SDMMC2",   "DLYBSD2"   },
    {"OCTOSPI1", "DLYBOS1"   },
    {"OCTOSPI2", "DLYBOS2"   },
});
/** @brief STM32U5 GTZC1 and GTZC2, RM0456 5.7.4 to 5.7.6 and 5.8: the SVD instance names of
 *         UART4, the instruction cache, the OTG, the FMC and the register blocks, the bits
 *         that RM0456 Table 5 gives to two blocks, and each delay block with the peripheral
 *         RM0456 Table 297 associates it with. */
inline constexpr gtzc_variant u5_gtzc{u5_gtzc_renames, u5_gtzc_companions};

inline constexpr auto h5_gtzc_renames    = std::to_array<gtzc_rename>({
    {"ADC12",     "ADC1"   },
    {"DAC1",      "DAC"    },
    {"I3C1",      "I3C"    },
    {"LPUART1",   "LPUART" },
    {"OCTOSPI1",  "OCTOSPI"},
    {"UCPD",      "UCPD1"  },
    {"FLASH_REG", "FLASH"  },
});
inline constexpr auto h5_gtzc_companions = std::to_array<gtzc_companion>({
    {"DCMI",    "PSSI"   },
    {"ADC1",    "ADC2"   },
    {"ADC1",    "ADCC"   },
    {"SDMMC1",  "DLYBSD1"},
    {"SDMMC2",  "DLYBSD2"},
    {"OCTOSPI", "DLYBOS1"},
});
/** @brief Names a USB full-speed device controller gives the register fields whose spelling
 *         differs between the variants. */
struct usb_drd_names
{
  /** @brief Endpoint register prefix, the register of endpoint n is `<prefix><n>R`. */
  std::string_view endpoint;
  /** @brief Transmit status, toggled by a written 1. */
  std::string_view stat_tx;
  /** @brief Transmit data toggle, toggled by a written 1. */
  std::string_view dtog_tx;
  /** @brief Transmit transfer done, cleared by a written 0. */
  std::string_view done_tx;
  /** @brief Endpoint kind. */
  std::string_view kind;
  /** @brief Endpoint type. */
  std::string_view type;
  /** @brief Receive status, toggled by a written 1. */
  std::string_view stat_rx;
  /** @brief Receive data toggle, toggled by a written 1. */
  std::string_view dtog_rx;
  /** @brief Receive transfer done, cleared by a written 0. */
  std::string_view done_rx;
  /** @brief ISTR endpoint identifier. */
  std::string_view endpoint_id;
  /** @brief ISTR bus reset flag. */
  std::string_view reset_done;
  /** @brief CNTR forced reset. */
  std::string_view core_reset;
  /** @brief CNTR suspend. */
  std::string_view suspend;
  /** @brief CNTR suspend ready or low-power mode. */
  std::string_view suspend_ready;
  /** @brief CNTR LPM L1 resume request. */
  std::string_view l1_resume;
  /** @brief CNTR L2 resume request. */
  std::string_view l2_resume;
  /** @brief CNTR host mode. */
  std::string_view host_mode;
};

/** @brief Differences between the USB full-speed device controllers the usb_drd rows serve. */
struct usb_drd_variant
{
  /** @brief Field names. */
  usb_drd_names names;
  /** @brief Endpoint register fields cleared by a written 0. */
  std::span<std::string_view const> endpoint_clears;
  /** @brief ISTR fields cleared by a written 0. */
  std::span<std::string_view const> status_clears;
  /** @brief NVIC line of every enabled ISTR flag. */
  std::string_view irq;
  /** @brief LPM register. */
  std::string_view lpm_register;
  /** @brief Battery charging detection register. */
  std::string_view charging_register;
  /** @brief Register of the D+ pull-up enable. */
  std::string_view pull_up_register;
  /** @brief Field of the D+ pull-up enable. */
  std::string_view pull_up_field;
  /** @brief Offset of the packet memory window in the controller's block. */
  std::uint32_t pma_offset;
  /** @brief Packet memory bytes as the controller addresses them. */
  std::uint32_t pma_bytes;
  /** @brief Manual section of the registers, for configuration errors. */
  std::string_view manual;
};

inline constexpr auto h5_usb_endpoint_clears = std::to_array<std::string_view>(
    {"VTTX", "VTRX", "NAK", "ERR_TX", "ERR_RX", "THREE_ERR_TX", "THREE_ERR_RX"});
inline constexpr auto h5_usb_status_clears = std::to_array<std::string_view>(
    {"L1REQ", "ESOF", "SOF", "RST_DCON", "SUSP", "WKUP", "ERR", "PMAOVR", "THR512", "DDISC"});
/** @brief STM32H5 USB, `usbfs_v1`, RM0481 section 61: CHEPnR, 2048 bytes of packet memory at
 *         offsets 0x400 to 0xBFF read by word, the pull-up in BCDR, LPM, battery charging
 *         detection and host mode. */
inline constexpr usb_drd_variant h5_usb{
    .names =
        {.endpoint      = "CHEP",
                .stat_tx       = "STATTX",
                .dtog_tx       = "DTOGTX",
                .done_tx       = "VTTX",
                .kind          = "EPKIND",
                .type          = "UTYPE",
                .stat_rx       = "STATRX",
                .dtog_rx       = "DTOGRX",
                .done_rx       = "VTRX",
                .endpoint_id   = "IDN",
                .reset_done    = "RST_DCON",
                .core_reset    = "USBRST",
                .suspend       = "SUSPEN",
                .suspend_ready = "SUSPRDY",
                .l1_resume     = "L1RES",
                .l2_resume     = "L2RES",
                .host_mode     = "HOST"},
    .endpoint_clears   = h5_usb_endpoint_clears,
    .status_clears     = h5_usb_status_clears,
    .irq               = "USB_DRD_FS_IRQn",
    .lpm_register      = "LPMCSR",
    .charging_register = "BCDR",
    .pull_up_register  = "BCDR",
    .pull_up_field     = "DPPU_DPD",
    .pma_offset        = 0x400,
    .pma_bytes         = 0x800,
    .manual            = "RM0481 61.6"
};

/** @brief STM32H562, H563 and H573 GTZC1, RM0481 5.7.4 to 5.7.6 and 5.8: the SVD names
 *         `DAC`, `I3C`, `LPUART`, `OCTOSPI` and `UCPD1` of the stems `DAC1`, `I3C1`,
 *         `LPUART1`, `OCTOSPI1` and `UCPD`, and `ADC12SEC` covering ADC1, ADC2 and their common
 *         registers `ADCC`, RM0481 Table 3, and each delay block with the peripheral RM0481
 *         Table 275 associates it with. `HDMICEC` names no peripheral of the SVD. */
inline constexpr gtzc_variant h5_gtzc{h5_gtzc_renames, h5_gtzc_companions};
}

#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Names a USB full-speed device controller gives the register fields whose spelling
 *         differs between the variants. An empty name is a field the variant lacks. */
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
  /** @brief NVIC line of a transfer done on an isochronous or double-buffered bulk endpoint,
   *         whatever CNTR.CTRM holds, empty for none. */
  std::string_view priority_irq;
  /** @brief LPM register, empty for none. */
  std::string_view lpm_register;
  /** @brief Battery charging detection register, empty for none. */
  std::string_view charging_register;
  /** @brief Register of the D+ pull-up enable inside the controller, empty when a peer gives
   *         it. */
  std::string_view pull_up_register;
  /** @brief Field of the D+ pull-up enable inside the controller. */
  std::string_view pull_up_field;
  /** @brief Peer whose usb_pull_up_source gives the pull-up enable, with no pull-up register. */
  std::string_view pull_up_peer;
  /** @brief Register holding the buffer descriptor table address, its field of the same name,
   *         empty for a table at packet memory address 0. */
  std::string_view descriptor_register;
  /** @brief Peripheral whose window is the packet memory, empty for a window inside the
   *         controller's own block. */
  std::string_view pma_peripheral;
  /** @brief Offset of the packet memory window in its peripheral. */
  std::uint32_t pma_offset;
  /** @brief Packet memory bytes as the controller addresses them. */
  std::uint32_t pma_bytes;
  /** @brief Packet memory bytes behind each 32-bit word of the CPU window, from its low lane
   *         up. */
  std::uint32_t pma_unit;
  /** @brief Bits of a buffer address the controller uses. */
  std::uint32_t address_mask;
  /** @brief CPU access sizes the manual allows on the packet memory, bit n for n bytes. */
  std::uint8_t pma_sizes;
  /** @brief EXTI line that follows ISTR.WKUP, none for no line. */
  std::optional<std::uint8_t> wakeup_line;
  /** @brief Whether the hardware owns the suspend bit: a written 0 keeps it, setting it clears
   *         FNR.LSOF, the ready bit follows it, a wake-up clears both. Otherwise the firmware owns
   *         it and a wake-up clears the low-power bit only. */
  bool hardware_suspend;
  /** @brief Whether the forced reset acts as a bus reset: endpoints and DADDR cleared and the
   *         bus reset flag set. Otherwise it clears the endpoints only. */
  bool forced_bus_reset;
  /** @brief Manual section of the registers, for configuration errors. */
  std::string_view manual;
};

inline constexpr auto h5_usb_endpoint_clears = std::to_array<std::string_view>(
    {"VTTX", "VTRX", "NAK", "ERR_TX", "ERR_RX", "THREE_ERR_TX", "THREE_ERR_RX"});
inline constexpr auto h5_usb_status_clears = std::to_array<std::string_view>(
    {"L1REQ", "ESOF", "SOF", "RST_DCON", "SUSP", "WKUP", "ERR", "PMAOVR", "THR512", "DDISC"});
/** @brief STM32H5 USB, `usbfs_v1`, RM0481 section 61: CHEPnR, 2048 bytes of packet memory at
 *         offsets 0x400 to 0xBFF read by word, the descriptor table at address 0, the pull-up in
 *         BCDR, LPM, battery charging detection and host mode. */
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
    .endpoint_clears     = h5_usb_endpoint_clears,
    .status_clears       = h5_usb_status_clears,
    .irq                 = "USB_DRD_FS_IRQn",
    .priority_irq        = "",
    .lpm_register        = "LPMCSR",
    .charging_register   = "BCDR",
    .pull_up_register    = "BCDR",
    .pull_up_field       = "DPPU_DPD",
    .pull_up_peer        = "",
    .descriptor_register = "",
    .pma_peripheral      = "",
    .pma_offset          = 0x400,
    .pma_bytes           = 0x800,
    .pma_unit            = 4,
    .address_mask        = 0xFFFC,
    .pma_sizes           = 1u << 4,
    .wakeup_line         = std::nullopt,
    .hardware_suspend    = true,
    .forced_bus_reset    = false,
    .manual              = "RM0481 61.6"
};

inline constexpr auto l1_usb_endpoint_clears =
    std::to_array<std::string_view>({"CTR_TX", "CTR_RX"});
inline constexpr auto l1_usb_status_clears =
    std::to_array<std::string_view>({"ESOF", "SOF", "RESET", "SUSP", "WKUP", "ERR", "PMAOVR"});
/** @brief STM32L1 USB, `usb1_v1_2_STM32L1_Cube`, RM0038 section 24: EPnR, 512 bytes of packet
 *         memory as 256 half-words, each in the low half of a word of the `USB_SRAM` window,
 *         RM0038 24.5.3, read by half-word or word, the descriptor table at BTABLE, the pull-up
 *         in SYSCFG_PMC.USB_PU, RM0038 24.4.2, USB_LP and USB_HP, RM0038 24.3.1, ISTR.WKUP on
 *         EXTI line 18, RM0038 Table 51. No LPM, battery charging detection or host mode. */
inline constexpr usb_drd_variant l1_usb{
    .names =
        {.endpoint      = "EP",
                .stat_tx       = "STAT_TX",
                .dtog_tx       = "DTOG_TX",
                .done_tx       = "CTR_TX",
                .kind          = "EP_KIND",
                .type          = "EP_TYPE",
                .stat_rx       = "STAT_RX",
                .dtog_rx       = "DTOG_RX",
                .done_rx       = "CTR_RX",
                .endpoint_id   = "EP_ID",
                .reset_done    = "RESET",
                .core_reset    = "FRES",
                .suspend       = "FSUSP",
                .suspend_ready = "LPMODE",
                .l1_resume     = "",
                .l2_resume     = "RESUME",
                .host_mode     = ""},
    .endpoint_clears     = l1_usb_endpoint_clears,
    .status_clears       = l1_usb_status_clears,
    .irq                 = "USB_LP_IRQn",
    .priority_irq        = "USB_HP_IRQn",
    .lpm_register        = "",
    .charging_register   = "",
    .pull_up_register    = "",
    .pull_up_field       = "",
    .pull_up_peer        = "SYSCFG",
    .descriptor_register = "BTABLE",
    .pma_peripheral      = "USB_SRAM",
    .pma_offset          = 0,
    .pma_bytes           = 0x200,
    .pma_unit            = 2,
    .address_mask        = 0xFFFE,
    .pma_sizes           = (1u << 2) | (1u << 4),
    .wakeup_line         = 18,
    .hardware_suspend    = false,
    .forced_bus_reset    = true,
    .manual              = "RM0038 24.5"
};

/** @brief Differences between the OTG_FS cores the otg_fs rows serve. */
struct otg_fs_variant
{
  /** @brief GRSTCTL bit 2, the frame counter reset. */
  std::string_view frame_counter_reset;
  /** @brief GRSTCTL bit 1, the partial soft reset, empty for none. */
  std::string_view partial_reset;
  /** @brief GPWRDN and GADPCTL present, claimed as storage registers. */
  bool adp;
};

/** @brief STM32U5 OTG_FS, GRSTCTL in RM0456 72.15.5. */
inline constexpr otg_fs_variant u5_otg_fs{
    .frame_counter_reset = "FSRST",
    .partial_reset       = "PSRST",
    .adp                 = false};

/** @brief STM32L47x/L48x OTG_FS, GRSTCTL in RM0351 47.15.5, no PSRST per Table 322. */
inline constexpr otg_fs_variant l4_otg_fs{
    .frame_counter_reset = "FCRST",
    .partial_reset       = "",
    .adp                 = true};

/**
 * @brief Factory of the USB OTG_FS device core model, RM0456 section 72 on the STM32U5, RM0351
 *        section 47 on the STM32L4.
 * @param hosts Board `usb_hosts`, each instance's virtual host, copied.
 * @param variant GRSTCTL reset field names of the core, copied.
 */
[[nodiscard]] auto
    otg_fs_factory(decltype(board::usb_hosts) const& hosts, otg_fs_variant const& variant)
        -> model_factory;

/**
 * @brief Factory of the USB full-speed device controller model, RM0481 section 61 on the STM32H5,
 *        RM0038 section 24 on the STM32L1.
 * @param hosts Board `usb_hosts`, each instance's virtual host, copied.
 * @param variant Register names, packet memory and pull-up of the controller, copied.
 */
[[nodiscard]] auto
    usb_drd_factory(decltype(board::usb_hosts) const& hosts, usb_drd_variant const& variant)
        -> model_factory;

/** @brief Peripheral name of the STM32L1 USB controller whose packet memory USB_SRAM serves. */
inline constexpr std::string_view usb_packet_memory_controller = "USB";

/**
 * @brief Factory of the packet memory window of a USB device controller held in a peripheral of
 *        its own, `USB_SRAM` on the STM32L1, served by the controller's model.
 * @param controller Peripheral name of the controller.
 */
[[nodiscard]] auto usb_packet_memory_factory(std::string_view controller) -> model_factory;

/** @brief The USB model rows. */
[[nodiscard]] auto usb_rows() -> std::span<builtin const>;
}

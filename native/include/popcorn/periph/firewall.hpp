#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>

namespace popcorn::periph
{
/**
 * @brief What a bus firewall decides for one access to a peripheral window. `allow` lets the
 *        access reach the model or the register layer. `read_zero_write_ignore` makes a read
 *        return 0 and drops a write: neither reaches the model nor the register layer, the trace
 *        and the watches still see the access, with 0 for a read and the written value for a
 *        write. `bus_error` fails the access with bus_error::denied: for a CPU access a precise
 *        BusFault at the instruction, the access not done, with the access address in BFAR, a
 *        transfer error for a DMA access, the error itself for a debugger access. A model's own
 *        bus error stays an imprecise BusFault at the next block boundary.
 */
enum class[[= reflex::derive{reflex::Format}]] bus_verdict : std::uint8_t
{
  allow,
  read_zero_write_ignore,
  bus_error,
};

/**
 * @brief What a bus firewall decides for one engine page of memory and one set of transaction
 *        attributes, when the engine maps the page. The engine keeps the verdict until the
 *        firewall's owner calls core::machine_services::memory_policy_changed.
 */
struct page_verdict
{
  /** @brief Every data load and store of the page with these attributes asks
   * bus_firewall::check_memory. Clear, they run at full speed with no check. */
  bool filter_data{};
  /** @brief An instruction fetch at the address that mapped the page is a BusFault. */
  bool deny_fetch{};
  /** @brief The page keeps no fetch verdict: each instruction fetch that misses the engine's
   * translated code asks bus_firewall::memory_page again with its own address. For a page whose
   * parts give different fetch verdicts. The engine translates code of the page per fetch
   * window, see fetch_window_shift. */
  bool fetch_each{};
  /** @brief With fetch_each, log2 of the size of the naturally aligned fetch window around the
   * mapping address, clipped to the page: every byte of it has the fetch verdict of that address
   * for these attributes. The engine then keeps translated code of the page, each block inside
   * the window of its first instruction, until the next memory_policy_changed. 0 runs the page one
   * instruction at a time. Values above 31 are clipped to 31. Read for an instruction fetch
   * only, a firewall leaves it 0 for a data access. */
  std::uint8_t fetch_window_shift{};

  /** @brief Member-wise equality. */
  friend auto operator==(page_verdict const&, page_verdict const&) -> bool = default;
};

/** @brief Bus addresses from @p first to @p last, both included. */
struct memory_extent
{
  /** @brief First address. */
  address first{};
  /** @brief Last address, included. */
  address last{};

  /** @brief Member-wise equality. */
  friend auto operator==(memory_extent const&, memory_extent const&) -> bool = default;
};

/**
 * @brief Security and privilege filter in front of every peripheral window of a machine, as an
 *        SoC bus firewall such as the STM32 GTZC TZSC. At most one per machine, installed by a
 *        model through model_context::install_firewall while the machine builds its models. It
 *        is called on the machine thread for every CPU, DMA and debugger access to a peripheral
 *        window, before the model and the register layer, so it must not take the GIL. It must
 *        not schedule events: the call skips the deadline update a model call makes.
 *
 *        A firewall whose filters_memory() is true also filters memory, RAM and flash: the
 *        engine asks memory_page() when it maps a page for some transaction attributes, then
 *        check_memory() for each data access of a page whose verdict filters data. Host
 *        accesses made by the CPU or DMA initiator, such as exception stacking and DMA
 *        transfers, ask check_memory() for every access. A debugger access asks it only with
 *        bus_attrs::filtered set, an access of a filtered core::memory_view.
 */
struct POPCORN_API bus_firewall
{
  /** @brief Creates the firewall. */
  bus_firewall() = default;
  /** @brief Not copyable, the machine holds its address. */
  bus_firewall(bus_firewall const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(bus_firewall const&) -> bus_firewall& = delete;
  /** @brief Not movable. */
  bus_firewall(bus_firewall&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(bus_firewall&&) -> bus_firewall& = delete;
  /** @brief Destroys the firewall. */
  virtual ~bus_firewall() = default;

  /**
   * @brief Decides one access to a peripheral window.
   * @param unit Index in part_description::peripherals of the peripheral the access reaches:
   *        the aliased peripheral for an access through a `SEC_` alias window.
   * @param attrs Attributes of the access. `secure` is false for every access on a machine
   *        without TrustZone, as access_ctx::secure.
   * @param op arch::memory_op::read or arch::memory_op::write, never arch::memory_op::fetch: no
   *        fetch reaches a peripheral window.
   * @return The verdict.
   */
  [[nodiscard]] virtual auto check_peripheral(std::size_t unit, bus_attrs attrs, arch::memory_op op)
      -> bus_verdict = 0;

  /**
   * @brief Whether the firewall filters memory too, asked once when the machine installs its
   *        engine hooks. False by default: memory runs with no check at all.
   */
  [[nodiscard]] virtual auto filters_memory() const -> bool
  {
    return false;
  }

  /**
   * @brief Whether check_peripheral() or check_memory() can fail an access of a DMA initiator
   *        with bus_verdict::bus_error, which ends the transfer with a transfer error, or raise an
   *        IRQ line while checking one. core::machine::horizon() then counts every active DMA
   * channel as able to raise a flag at its next burst. True by default.
   */
  [[nodiscard]] virtual auto faults_transfers() const -> bool
  {
    return true;
  }

  /**
   * @brief Decides one engine page of memory for one set of transaction attributes, when the
   *        engine maps it for a CPU access. The engine keeps the verdict until the owner calls
   *        core::machine_services::memory_policy_changed. Called only when filters_memory() is
   *        true, for RAM and flash pages, never for a peripheral window.
   * @param page First bus address of the page.
   * @param size Page size in bytes.
   * @param at Bus address of the access that maps the page, inside it. For a fetch the address
   *        of the fetched instruction.
   * @param attrs Attributes of the transaction, after the core's security attribution.
   * @param op Access that maps the page. An illegal fetch may raise its event only when @p op
   *        is arch::memory_op::fetch, the other calls map the page without a fetch. The BusFault
   *        of a denied fetch is raised after this call returns, so the event's interrupt is
   *        already pending when the core ranks the two.
   * @return The verdict for the page and @p attrs, whatever @p op is.
   */
  [[nodiscard]] virtual auto memory_page(
      [[maybe_unused]] address         page,
      [[maybe_unused]] std::uint32_t   size,
      [[maybe_unused]] address         at,
      [[maybe_unused]] bus_attrs       attrs,
      [[maybe_unused]] arch::memory_op op) -> page_verdict
  {
    return {};
  }

  /**
   * @brief Decides one data access to memory: a CPU access of a page whose verdict filters
   *        data, a host access by the CPU or DMA initiator, or a filtered debugger access.
   *        Called only when filters_memory() is true.
   * @param at Bus address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param attrs Attributes of the transaction.
   * @param op arch::memory_op::read or arch::memory_op::write.
   * @return The verdict, as for a peripheral window: `read_zero_write_ignore` reads 0 and drops
   *         the write, `bus_error` fails the access.
   */
  [[nodiscard]] virtual auto check_memory(
      [[maybe_unused]] address         at,
      [[maybe_unused]] std::uint8_t    size,
      [[maybe_unused]] bus_attrs       attrs,
      [[maybe_unused]] arch::memory_op op) -> bus_verdict
  {
    return bus_verdict::allow;
  }

  /**
   * @brief The addresses around an access check_memory() just allowed over which every access
   *        with the same attributes and operation is allowed too, with no event, until the next
   *        core::machine_services::memory_policy_changed. A host bus master keeps it in a
   *        core::memory_grant to skip check_memory() for later accesses. Called only when
   *        filters_memory() is true.
   * @param at Bus address of the allowed access.
   * @param size Its size in bytes.
   * @param attrs Its attributes.
   * @param op Its operation.
   * @return The extent, at least the access itself, which the default returns.
   */
  [[nodiscard]] virtual auto allowed_extent(
      address                          at,
      std::uint8_t                     size,
      [[maybe_unused]] bus_attrs       attrs,
      [[maybe_unused]] arch::memory_op op) -> memory_extent
  {
    return {.first = at, .last = at + size - 1};
  }
};
}

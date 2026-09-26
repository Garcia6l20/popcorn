#pragma once

#include <popcorn/periph/model.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::testing
{
/**
 * @brief Native model of the core test description's `TESTDEV`. Claims `DATA` and `CTRL`,
 *        stores writes as they come, and on a write of 1 to `CTRL.GO` raises its IRQ line after
 *        delay instructions through its event port. A read of `DATA` lowers the line. Records
 *        every access, and on request fails or throws on reads, requests a stop on writes, and
 *        on a `CTRL.GO` write patches a halfword through DMA or halves the core clock. go_delay
 *        replaces the delay, and data_on_event sets `DATA` to 1 when the event fires.
 *        Test-only.
 */
class test_model final : public periph::model
{
public:
  /** @brief Offset of `DATA` in `TESTDEV`. */
  static constexpr std::uint32_t data_offset = 0x58;
  /** @brief Offset of `CTRL` in `TESTDEV`. */
  static constexpr std::uint32_t ctrl_offset = 0x5c;
  /** @brief Instructions between a `CTRL.GO` write and the IRQ. */
  static constexpr std::uint64_t delay = 100;

  /**
   * @brief Creates the model over its context, which it keeps.
   * @param ctx Context of `TESTDEV`.
   */
  explicit test_model(periph::model_context& ctx) noexcept : context_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override;
  void               reset(periph::register_file& regs, reset_kind kind) override;
  auto               on_read(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      periph::access_ctx     ctx) -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      periph::register_file& regs,
      std::uint32_t          offset,
      std::uint8_t           size,
      std::uint32_t          old,
      std::uint32_t          value,
      periph::access_ctx     ctx) -> std::expected<void, bus_error> override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  /** @brief Context the model was created with. */
  [[nodiscard]] auto context() const noexcept -> periph::model_context&;

  /** @brief One access the model served. */
  struct access
  {
    /** @brief Byte offset in the peripheral. */
    std::uint32_t offset{};
    /** @brief Access size in bytes. */
    std::uint8_t size{};
    /** @brief Whether it was a write. */
    bool write{};
    /** @brief Stored bytes before a write. */
    std::uint32_t old{};
    /** @brief Value written, 0 for a read. */
    std::uint32_t value{};
    /** @brief Context of the access. */
    periph::access_ctx ctx{};
  };

  /** @brief Whether accesses are recorded, off for benchmarks. */
  bool record = true;
  /** @brief Accesses served, in order. */
  std::vector<access> accesses{};
  /** @brief Whether reads return bus_error::denied. */
  bool fail_reads = false;
  /** @brief Whether writes return bus_error::denied. */
  bool fail_writes = false;
  /** @brief Whether reads throw a std::runtime_error naming their rank. */
  bool throw_reads = false;
  /** @brief Whether writes request a stop with detail "model stop". */
  bool stop_on_write = false;
  /** @brief Address a `CTRL.GO` write patches through DMA with patch_value. */
  std::optional<address> patch_on_go{};
  /** @brief Halfword the patch writes. */
  std::uint16_t patch_value{};
  /** @brief Whether a `CTRL.GO` write halves the core clock. */
  bool halve_rate_on_go = false;

  /** @brief Instruction count at which the delayed IRQ event last fired. */
  std::optional<icount> fired_at{};

  /** @brief Instructions between a `CTRL.GO` write and its event, delay when empty. */
  std::optional<std::uint64_t> go_delay{};
  /** @brief Whether the `CTRL.GO` event also sets `DATA` to 1. */
  bool data_on_event = false;

  /** @brief Whether load() throws a std::runtime_error. */
  bool throw_load = false;

  /** @brief Whether reset() raises the IRQ line, for the reset order test. */
  bool raise_in_reset = false;
  /** @brief Kind of the last reset, empty before the first. */
  std::optional<reset_kind> last_reset{};
  /** @brief Whether on_write, on_event and reset log a model_warning through the services: pc
   * left 0 for the machine to fill, except event_pc from on_event. */
  bool log_messages = false;
  /** @brief pc the on_event record carries. */
  static constexpr address event_pc = 0x0800'1234;

private:
  void note(std::string message, address pc = 0) const;

  periph::model_context& context_;
};

/** @brief Factory creating a test_model. */
auto test_model_factory() -> periph::model_factory;
}

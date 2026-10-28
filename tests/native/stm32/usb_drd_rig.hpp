#pragma once

#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::testing::usb_drd
{
namespace usb = popcorn::periph::usb;

inline constexpr std::string_view firmware = "h573_rcc_ll";
inline constexpr std::uint32_t    irq      = 74;
inline constexpr std::uint64_t    us       = 1'000'000;
inline constexpr std::uint32_t    pma      = 0x400;

inline constexpr std::uint32_t disabled = 0;
inline constexpr std::uint32_t stall    = 1;
inline constexpr std::uint32_t nak      = 2;
inline constexpr std::uint32_t valid    = 3;

inline constexpr std::uint32_t bulk    = 0;
inline constexpr std::uint32_t control = 1;
inline constexpr std::uint32_t iso     = 2;

struct recording_host final : usb::host_side
{
  std::vector<std::pair<bool, std::uint64_t>> changes;
  std::vector<std::byte>                      memory;

  void pull_up_changed(bool on, vtime at) override
  {
    changes.emplace_back(on, at.ps);
  }

  void on_time(vtime) override
  {}

  void save(std::vector<std::byte>& out) const override
  {
    out.insert(out.end(), memory.begin(), memory.end());
  }

  void load(std::span<std::byte const> in) override
  {
    memory.assign(in.begin(), in.end());
  }
};

struct endpoint_setup
{
  std::uint32_t address{};
  std::uint32_t type{bulk};
  std::uint32_t stat_tx{disabled};
  std::uint32_t stat_rx{disabled};
  std::uint32_t kind{};
  std::uint32_t dtog_tx{};
  std::uint32_t dtog_rx{};
};

struct rig
{
  std::unique_ptr<stm32_fixture>  f;
  std::shared_ptr<recording_host> host = std::make_shared<recording_host>();

  explicit rig(bool strict = true, part_description const& part = h573())
  {
    auto options               = stm32_options();
    options.strict_peripherals = strict;
    f = std::make_unique<stm32_fixture>(part, firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
  }

  explicit rig(popcorn::periph::stm32::board const& board)
  {
    auto options               = stm32_options();
    options.strict_peripherals = true;
    auto factories             = popcorn::periph::stm32::factories(board, h573());
    options.models.emplace("USB", std::move(factories.at("USB")));
    f = std::make_unique<stm32_fixture>(h573(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
  }

  [[nodiscard]] auto cable() const -> popcorn::periph::stm32::usb_host_port&
  {
    auto* const found = f->model_of<popcorn::periph::stm32::usb_host_port>("USB");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto port() const -> usb::device_port&
  {
    auto* const found = f->model_of<usb::device_port>("USB");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto reg(std::string_view name) const -> register_desc const&
  {
    auto const& registers = f->target->peripheral("USB").description().registers;
    auto const  found     = std::ranges::find(registers, name, &register_desc::name);
    REQUIRE(found != registers.end());
    return *found;
  }

  [[nodiscard]] auto
      mask(std::string_view name, std::string_view field, std::uint32_t value = 1) const
      -> std::uint32_t
  {
    auto const& fields = reg(name).fields;
    auto const  found  = std::ranges::find(fields, field, &field_desc::name);
    REQUIRE(found != fields.end());
    return value << found->lsb;
  }

  [[nodiscard]] auto read_at(std::uint32_t offset, std::uint8_t size = 4) const -> std::uint32_t
  {
    auto&      regs = f->target->peripheral("USB");
    auto const value =
        f->target->model("USB")->on_read(regs, offset, size, {.pc = 0, .now = icount{0}});
    REQUIRE(value.has_value());
    return *value;
  }

  void write_at(std::uint32_t offset, std::uint32_t value, std::uint8_t size = 4) const
  {
    auto& regs = f->target->peripheral("USB");
    REQUIRE(f->target->model("USB")
                ->on_write(
                    regs, offset, size, regs.peek(offset & ~3u), value, {.pc = 0, .now = icount{0}})
                .has_value());
  }

  void write_unchecked(std::string_view name, std::uint32_t value) const
  {
    auto&      regs   = f->target->peripheral("USB");
    auto const offset = reg(name).offset;
    static_cast<void>(f->target->model("USB")->on_write(
        regs, offset, 4, regs.peek(offset), value, {.pc = 0, .now = icount{0}}));
  }

  [[nodiscard]] auto read(std::string_view name) const -> std::uint32_t
  {
    return read_at(reg(name).offset);
  }

  void write(std::string_view name, std::uint32_t value) const
  {
    write_at(reg(name).offset, value);
  }

  [[nodiscard]] auto get(std::string_view name, std::string_view field) const -> std::uint32_t
  {
    return f->field("USB", name, field);
  }

  [[nodiscard]] auto pma_word(std::uint32_t at) const -> std::uint32_t
  {
    return read_at(pma + at);
  }

  void set_pma_word(std::uint32_t at, std::uint32_t word) const
  {
    write_at(pma + at, word);
  }

  void tx_buffer(std::size_t n, std::uint32_t address, std::uint32_t count, std::uint32_t which = 0)
      const
  {
    set_pma_word(static_cast<std::uint32_t>(8 * n + 4 * which), (count << 16) | address);
  }

  void rx_buffer(std::size_t n, std::uint32_t address, std::uint32_t bytes, std::uint32_t which = 1)
      const
  {
    auto const large  = bytes > 62;
    auto const blocks = large ? bytes / 32 - 1 : bytes / 2;
    set_pma_word(
        static_cast<std::uint32_t>(8 * n + 4 * which),
        (large ? 1u << 31 : 0u) | (blocks << 26) | address);
  }

  [[nodiscard]] auto rx_count(std::size_t n, std::uint32_t which = 1) const -> std::uint32_t
  {
    return (pma_word(static_cast<std::uint32_t>(8 * n + 4 * which)) >> 16) & 0x3ffu;
  }

  void put_bytes(std::uint32_t at, std::span<std::uint8_t const> bytes) const
  {
    for(std::size_t index = 0; index < bytes.size(); index += 4)
    {
      std::uint32_t word = 0;
      for(std::size_t lane = 0; lane < 4 and index + lane < bytes.size(); ++lane)
      {
        word |= std::uint32_t{bytes[index + lane]} << (8 * lane);
      }
      set_pma_word(at + static_cast<std::uint32_t>(index), word);
    }
  }

  [[nodiscard]] auto bytes_at(std::uint32_t at, std::size_t count) const
      -> std::vector<std::uint8_t>
  {
    std::vector<std::uint8_t> out;
    for(std::size_t index = 0; index < count; ++index)
    {
      auto const word = pma_word(at + static_cast<std::uint32_t>(index & ~std::size_t{3}));
      out.push_back(static_cast<std::uint8_t>(word >> (8 * (index % 4))));
    }
    return out;
  }

  void endpoint(std::size_t n, endpoint_setup const& want) const
  {
    auto const name    = std::format("CHEP{}R", n);
    auto const now     = read(name);
    auto const toggled = [&](std::string_view field, std::uint32_t value) {
      auto const width_mask = mask(name, field, field.starts_with("STAT") ? 3u : 1u);
      return (now & width_mask) ^ mask(name, field, value);
    };
    write(
        name, mask(name, "EA", want.address)
                  | mask(name, "UTYPE", want.type)
                  | mask(name, "EPKIND", want.kind)
                  | mask(name, "VTRX")
                  | mask(name, "VTTX")
                  | toggled("STATTX", want.stat_tx)
                  | toggled("STATRX", want.stat_rx)
                  | toggled("DTOGTX", want.dtog_tx)
                  | toggled("DTOGRX", want.dtog_rx));
  }

  void clear_flags(std::size_t n) const
  {
    auto const name = std::format("CHEP{}R", n);
    write(
        name, read(name)
                  & ~(mask(name, "VTRX")
                      | mask(name, "VTTX")
                      | mask(name, "STATTX", 3)
                      | mask(name, "STATRX", 3)
                      | mask(name, "DTOGTX")
                      | mask(name, "DTOGRX")));
  }

  [[nodiscard]] auto now() const -> vtime
  {
    return f->target->virtual_clock().time();
  }

  void run_for(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop(
             {.max_instructions = icount{1'000'000'000}, .until_time = vtime{now().ps + ps}})
            .reason
        == stop_reason::time);
  }

  [[nodiscard]] auto pending() const -> bool
  {
    auto const word = f->target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return ((*word >> (irq % 32)) & 1u) != 0;
  }

  void power_up() const
  {
    write("CNTR", 0);
    write("DADDR", mask("DADDR", "EF"));
  }

  void connect() const
  {
    power_up();
    port().attach(host);
    write("BCDR", mask("BCDR", "DPPU_DPD"));
    REQUIRE(port().pulled_up());
  }

  void open_control() const
  {
    tx_buffer(0, 0x40, 0);
    rx_buffer(0, 0x80, 64);
    endpoint(0, {.address = 0, .type = control, .stat_tx = nak, .stat_rx = valid});
  }

  void reset_bus() const
  {
    port().bus_reset(now());
    run_for(10'000 * us);
    port().bus_reset_end(now());
  }

  void enumerate() const
  {
    connect();
    reset_bus();
    run_for(20 * us);
    write("ISTR", 0);
    write("DADDR", mask("DADDR", "EF"));
    open_control();
  }
};
}

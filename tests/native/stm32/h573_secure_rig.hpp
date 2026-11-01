#pragma once

#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/flash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::testing
{
/** @brief A model that does nothing and hands out its machine's bus services. */
class bus_probe final : public periph::model
{
public:
  explicit bus_probe(periph::model_context& ctx) : ctx_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0;
  }

  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return ctx_.services;
  }

private:
  periph::model_context& ctx_;
};

/** @brief An STM32H573 at TZEN=1 on a firmware parked in an SRAM loop, the SAU at ALLNS, with
 *         Secure accesses through the 0x5xxx alias and Non-secure ones through the 0x4xxx
 *         address, both privileged debug accesses of that state. */
struct h573_secure_rig
{
  static constexpr std::uint32_t parked_at = 0x2000'0100;

  bus_probe*    bus{};
  stm32_fixture f;

  explicit h573_secure_rig(
      std::string_view                   firmware = "h573_pwr_ll",
      periph::stm32::option_bytes const& wanted   = {.tzen = true},
      arch::fault_policy                 faults   = arch::fault_policy::stop)
      : f{h573(), firmware, with_probe(options(wanted, faults))}
  {
    REQUIRE(f.target->mem().write(parked_at, 4, 0xE7FD'BF30).has_value());
    f.set("pc", parked_at | 1u);
    REQUIRE(f.target->mem(true).write(0xe000'edd0, 4, 0x2).has_value());
  }

  auto with_probe(core::machine_options out) -> core::machine_options
  {
    out.models.emplace(
        "OTFDEC1", [this](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          auto made = std::make_shared<bus_probe>(ctx);
          bus       = made.get();
          return made;
        });
    return out;
  }

  static auto options(
      periph::stm32::option_bytes const& wanted,
      arch::fault_policy                 faults = arch::fault_policy::stop) -> core::machine_options
  {
    auto out   = stm32_options();
    out.faults = faults;
    REQUIRE(periph::stm32::configure_option_bytes(h573(), wanted, out).has_value());
    return out;
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view name) const
      -> popcorn::address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const* found = popcorn::find_register(desc, name);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto
      bit(std::string_view peripheral, std::string_view reg, std::string_view field) const
      -> std::uint32_t
  {
    auto const* found = popcorn::find_register(f.target->peripheral(peripheral).description(), reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    return ((std::uint32_t{1} << bits->width) - 1) << bits->lsb;
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg, bool secure) const
      -> std::uint32_t
  {
    auto const value = f.target->mem(secure).read(alias(peripheral, reg, secure), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value, bool secure)
      const
  {
    REQUIRE(f.target->mem(secure).write(alias(peripheral, reg, secure), 4, value).has_value());
  }

  void write_as(
      std::string_view   peripheral,
      std::string_view   reg,
      std::uint32_t      value,
      popcorn::bus_attrs attrs) const
  {
    REQUIRE(bus->services().write(at(peripheral, reg), 4, value, attrs).has_value());
  }

  [[nodiscard]] auto
      read_as(std::string_view peripheral, std::string_view reg, popcorn::bus_attrs attrs) const
      -> std::uint32_t
  {
    auto const value = bus->services().read(at(peripheral, reg), 4, attrs);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto illegal_flag(std::string_view field) const -> bool
  {
    auto const& desc = f.target->peripheral("GTZC1_TZIC").description();
    for(auto const& reg : desc.registers)
    {
      auto const bits = std::ranges::find(reg.fields, field, &popcorn::field_desc::name);
      if(bits != reg.fields.end())
      {
        auto const word = f.target->peripheral("GTZC1_TZIC").peek(reg.offset & ~3u);
        return ((word >> bits->lsb) & 1u) != 0;
      }
    }
    FAIL("no TZIC field " << field);
    return false;
  }

  [[nodiscard]] auto alias(std::string_view peripheral, std::string_view reg, bool secure) const
      -> popcorn::address
  {
    return at(peripheral, reg) + (secure ? 0x1000'0000u : 0u);
  }

  [[nodiscard]] auto peek(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const* found = popcorn::find_register(f.target->peripheral(peripheral).description(), reg);
    REQUIRE(found != nullptr);
    return f.target->peripheral(peripheral).peek(found->offset & ~3u);
  }
};
}

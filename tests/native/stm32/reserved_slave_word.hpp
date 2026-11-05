#pragma once

#include "../support/inert_model.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::testing
{
/** @brief Model that hands its context to a test, so the test makes CPU accesses. */
class reserved_word_port final : public inert_model
{
public:
  explicit reserved_word_port(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;
};

/**
 * @brief Checks that each timer of @p units, none with an SMCR, serves offset 0x08: a CPU read
 *        gives 0 and logs nothing, a debugger write is dropped and logs nothing, a CPU write is
 *        dropped and logs one warning per timer, and under strict_peripherals the CPU write
 *        raises model_error, with and without strict.
 * @param desc The part.
 * @param firmware A firmware of the part with an `idle` loop.
 * @param host A peripheral whose model the port replaces.
 * @param units Timers with no SMCR in @p desc.
 */
inline void serves_reserved_slave_word(
    part_description const&                 desc,
    std::string_view                        firmware,
    std::string_view                        host,
    std::initializer_list<std::string_view> units)
{
  for(auto const strict : {true, false})
  {
    CAPTURE(strict);
    auto options               = stm32_options();
    options.models             = periph::stm32::factories({}, desc);
    options.strict_peripherals = strict;
    options.models.insert_or_assign(
        std::string{host}, [](periph::model_context& ctx, peripheral_desc const&) {
          return std::make_shared<reserved_word_port>(ctx);
        });
    stm32_fixture                 f{desc, firmware, std::move(options)};
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    f.set("pc", sym(firmware, "idle"));
    auto* const port = f.model_of<reserved_word_port>(host);
    REQUIRE(port != nullptr);
    auto const warnings = [&] {
      return static_cast<std::size_t>(
          std::ranges::count(logged, core::log_kind::model_warning, &core::log_record::kind));
    };
    std::size_t expected = 0;
    for(auto const unit : units)
    {
      CAPTURE(unit);
      auto const& peripheral = f.target->peripheral(unit).description();
      REQUIRE(find_register(peripheral, "SMCR") == nullptr);
      auto const at    = peripheral.base + 8u;
      auto const first = port->context.services.read(at, 4, initiator::cpu);
      REQUIRE(first.has_value());
      CHECK(*first == 0);
      CHECK(warnings() == expected);
      CHECK(port->context.services.write(at, 4, 0xA5u, initiator::debugger).has_value());
      CHECK(warnings() == expected);
      auto const debugged = port->context.services.read(at, 4, initiator::cpu);
      REQUIRE(debugged.has_value());
      CHECK(*debugged == 0);
      for(auto const value : {0x5Au, 0xFFFFu})
      {
        if(strict)
        {
          CHECK_THROWS_AS(
              static_cast<void>(port->context.services.write(at, 4, value, initiator::cpu)),
              model_error);
        }
        else
        {
          CHECK(port->context.services.write(at, 4, value, initiator::cpu).has_value());
        }
      }
      ++expected;
      CHECK(warnings() == expected);
      auto const after = port->context.services.read(at, 4, initiator::cpu);
      REQUIRE(after.has_value());
      CHECK(*after == 0);
      CHECK(warnings() == expected);
    }
  }
}
}

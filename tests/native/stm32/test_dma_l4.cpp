#include "../support/inert_model.hpp"
#include "dma.hpp"
#include "dma_rows.hpp"
#include "dma_signal.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::reset_kind;
using popcorn::stop_reason;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;

constexpr auto          firmware   = std::string_view{"l476_l4_dma_ll"};
constexpr address       dma1       = 0x4002'0000;
constexpr address       dma2       = 0x4002'0400;
constexpr address       isr        = 0x00;
constexpr address       ifcr       = 0x04;
constexpr address       cselr      = 0xA8;
constexpr address       source     = 0x2000'2000;
constexpr address       target     = 0x2000'2400;
constexpr std::uint32_t en         = 1u << 0;
constexpr std::uint32_t word_sizes = (2u << 8) | (2u << 10);
constexpr std::uint64_t latency_ps = 4'000'000'000'000 / 4'000'000;

constexpr auto ccr(std::uint32_t ch) -> address
{
  return 0x08 + 20 * (ch - 1);
}

constexpr auto cndtr(std::uint32_t ch) -> address
{
  return ccr(ch) + 4;
}

class requester final : public popcorn::testing::inert_model, public stm32::dma_requester
{
public:
  requester(periph::model_context& ctx, std::vector<std::string> names) : names_{std::move(names)}
  {
    for(auto const& name : names_)
    {
      signals_.emplace_back(ctx.peers, name);
    }
  }

  std::vector<std::string> acks;
  bool                     drop_on_ack = true;

  void set(std::string_view name, bool level)
  {
    auto const found = std::ranges::find(names_, name);
    REQUIRE(found != names_.end());
    signals_[static_cast<std::size_t>(found - names_.begin())].set(level);
  }

  void acknowledged(stm32::dma_controller const& by, std::uint32_t request) override
  {
    for(std::size_t index = 0; index < signals_.size(); ++index)
    {
      if(signals_[index].carries(by, request))
      {
        acks.push_back(names_[index]);
        if(drop_on_ack)
        {
          signals_[index].set(false);
        }
      }
    }
  }

  void transfer_completed(stm32::dma_controller const&, std::uint32_t) override
  {}
  void last_but_one(stm32::dma_controller const&, std::uint32_t) override
  {}
  void count_ended(stm32::dma_controller const&, std::uint32_t) override
  {}

  void reset(periph::register_file&, reset_kind) override
  {
    for(auto& one : signals_)
    {
      one.resolve();
    }
  }

private:
  std::vector<std::string>       names_;
  std::vector<stm32::dma_signal> signals_;
};

struct table_row
{
  std::string_view instance;
  std::uint32_t    channel;
  std::uint32_t    code;
  std::string_view signal;
};

constexpr auto rm0351_requests = std::to_array<table_row>({
    {"DMA1", 1, 0, "adc1_dma"       },
    {"DMA1", 1, 4, "tim2_cc3_dma"   },
    {"DMA1", 1, 5, "tim17_cc1_dma"  },
    {"DMA1", 1, 5, "tim17_upd_dma"  },
    {"DMA1", 1, 6, "tim4_cc1_dma"   },
    {"DMA1", 2, 0, "adc2_dma"       },
    {"DMA1", 2, 1, "spi1_rx_dma"    },
    {"DMA1", 2, 2, "usart3_tx_dma"  },
    {"DMA1", 2, 3, "i2c3_tx_dma"    },
    {"DMA1", 2, 4, "tim2_upd_dma"   },
    {"DMA1", 2, 5, "tim3_cc3_dma"   },
    {"DMA1", 2, 7, "tim1_cc1_dma"   },
    {"DMA1", 3, 0, "adc3_dma"       },
    {"DMA1", 3, 1, "spi1_tx_dma"    },
    {"DMA1", 3, 2, "usart3_rx_dma"  },
    {"DMA1", 3, 3, "i2c3_rx_dma"    },
    {"DMA1", 3, 4, "tim16_cc1_dma"  },
    {"DMA1", 3, 4, "tim16_upd_dma"  },
    {"DMA1", 3, 5, "tim3_cc4_dma"   },
    {"DMA1", 3, 5, "tim3_upd_dma"   },
    {"DMA1", 3, 6, "tim6_upd_dma"   },
    {"DMA1", 3, 6, "dac1_ch1_dma"   },
    {"DMA1", 3, 7, "tim1_cc2_dma"   },
    {"DMA1", 4, 0, "dfsdm1_flt0_dma"},
    {"DMA1", 4, 1, "spi2_rx_dma"    },
    {"DMA1", 4, 2, "usart1_tx_dma"  },
    {"DMA1", 4, 3, "i2c2_tx_dma"    },
    {"DMA1", 4, 5, "tim7_upd_dma"   },
    {"DMA1", 4, 5, "dac1_ch2_dma"   },
    {"DMA1", 4, 6, "tim4_cc2_dma"   },
    {"DMA1", 4, 7, "tim1_cc4_dma"   },
    {"DMA1", 4, 7, "tim1_trg_dma"   },
    {"DMA1", 4, 7, "tim1_com_dma"   },
    {"DMA1", 5, 0, "dfsdm1_flt1_dma"},
    {"DMA1", 5, 1, "spi2_tx_dma"    },
    {"DMA1", 5, 2, "usart1_rx_dma"  },
    {"DMA1", 5, 3, "i2c2_rx_dma"    },
    {"DMA1", 5, 4, "tim2_cc1_dma"   },
    {"DMA1", 5, 5, "quadspi_dma"    },
    {"DMA1", 5, 6, "tim4_cc3_dma"   },
    {"DMA1", 5, 7, "tim15_cc1_dma"  },
    {"DMA1", 5, 7, "tim15_upd_dma"  },
    {"DMA1", 5, 7, "tim15_trg_dma"  },
    {"DMA1", 5, 7, "tim15_com_dma"  },
    {"DMA1", 6, 0, "dfsdm1_flt2_dma"},
    {"DMA1", 6, 1, "sai2_a_dma"     },
    {"DMA1", 6, 2, "usart2_rx_dma"  },
    {"DMA1", 6, 3, "i2c1_tx_dma"    },
    {"DMA1", 6, 4, "tim16_cc1_dma"  },
    {"DMA1", 6, 4, "tim16_upd_dma"  },
    {"DMA1", 6, 5, "tim3_cc1_dma"   },
    {"DMA1", 6, 5, "tim3_trg_dma"   },
    {"DMA1", 6, 7, "tim1_upd_dma"   },
    {"DMA1", 7, 0, "dfsdm1_flt3_dma"},
    {"DMA1", 7, 1, "sai2_b_dma"     },
    {"DMA1", 7, 2, "usart2_tx_dma"  },
    {"DMA1", 7, 3, "i2c1_rx_dma"    },
    {"DMA1", 7, 4, "tim2_cc2_dma"   },
    {"DMA1", 7, 4, "tim2_cc4_dma"   },
    {"DMA1", 7, 5, "tim17_cc1_dma"  },
    {"DMA1", 7, 5, "tim17_upd_dma"  },
    {"DMA1", 7, 6, "tim4_upd_dma"   },
    {"DMA1", 7, 7, "tim1_cc3_dma"   },
    {"DMA2", 1, 1, "sai1_a_dma"     },
    {"DMA2", 1, 2, "uart5_tx_dma"   },
    {"DMA2", 1, 3, "spi3_rx_dma"    },
    {"DMA2", 1, 4, "swpmi1_rx_dma"  },
    {"DMA2", 1, 5, "tim5_cc4_dma"   },
    {"DMA2", 1, 5, "tim5_trg_dma"   },
    {"DMA2", 1, 6, "aes_in_dma"     },
    {"DMA2", 1, 7, "tim8_cc3_dma"   },
    {"DMA2", 1, 7, "tim8_upd_dma"   },
    {"DMA2", 2, 1, "sai1_b_dma"     },
    {"DMA2", 2, 2, "uart5_rx_dma"   },
    {"DMA2", 2, 3, "spi3_tx_dma"    },
    {"DMA2", 2, 4, "swpmi1_tx_dma"  },
    {"DMA2", 2, 5, "tim5_cc3_dma"   },
    {"DMA2", 2, 5, "tim5_upd_dma"   },
    {"DMA2", 2, 6, "aes_out_dma"    },
    {"DMA2", 2, 7, "tim8_cc4_dma"   },
    {"DMA2", 2, 7, "tim8_trg_dma"   },
    {"DMA2", 2, 7, "tim8_com_dma"   },
    {"DMA2", 3, 0, "adc1_dma"       },
    {"DMA2", 3, 1, "sai2_a_dma"     },
    {"DMA2", 3, 2, "uart4_tx_dma"   },
    {"DMA2", 3, 4, "spi1_rx_dma"    },
    {"DMA2", 3, 6, "aes_out_dma"    },
    {"DMA2", 4, 0, "adc2_dma"       },
    {"DMA2", 4, 1, "sai2_b_dma"     },
    {"DMA2", 4, 3, "tim6_upd_dma"   },
    {"DMA2", 4, 3, "dac1_ch1_dma"   },
    {"DMA2", 4, 4, "spi1_tx_dma"    },
    {"DMA2", 4, 5, "tim5_cc2_dma"   },
    {"DMA2", 4, 7, "sdmmc1_dma"     },
    {"DMA2", 5, 0, "adc3_dma"       },
    {"DMA2", 5, 2, "uart4_rx_dma"   },
    {"DMA2", 5, 3, "tim7_upd_dma"   },
    {"DMA2", 5, 3, "dac1_ch2_dma"   },
    {"DMA2", 5, 5, "tim5_cc1_dma"   },
    {"DMA2", 5, 6, "aes_in_dma"     },
    {"DMA2", 5, 7, "sdmmc1_dma"     },
    {"DMA2", 6, 1, "sai1_a_dma"     },
    {"DMA2", 6, 2, "usart1_tx_dma"  },
    {"DMA2", 6, 4, "lpuart1_tx_dma" },
    {"DMA2", 6, 5, "i2c1_rx_dma"    },
    {"DMA2", 6, 7, "tim8_cc1_dma"   },
    {"DMA2", 7, 1, "sai1_b_dma"     },
    {"DMA2", 7, 2, "usart1_rx_dma"  },
    {"DMA2", 7, 3, "quadspi_dma"    },
    {"DMA2", 7, 4, "lpuart1_rx_dma" },
    {"DMA2", 7, 5, "i2c1_tx_dma"    },
    {"DMA2", 7, 7, "tim8_cc2_dma"   },
});

auto instance_of(std::string_view signal) -> std::string
{
  std::string out{signal.substr(0, signal.find('_'))};
  std::ranges::transform(out, out.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  return out;
}

auto has_unit(part_description const& desc, std::string_view name) -> bool
{
  return std::ranges::any_of(
      desc.peripherals, [name](popcorn::peripheral_desc const& unit) { return unit.name == name; });
}

using names_by_unit = std::map<std::string, std::vector<std::string>>;

auto with_requesters(
    names_by_unit const&  by_unit,
    core::machine_options options = popcorn::testing::stm32_options()) -> core::machine_options
{
  for(auto const& [unit, names] : by_unit)
  {
    if(has_unit(l476rc(), unit))
    {
      options.models.emplace(
          unit,
          [names](periph::model_context& ctx, popcorn::peripheral_desc const&)
              -> std::shared_ptr<periph::model> {
            return std::make_shared<requester>(ctx, names);
          });
    }
  }
  return options;
}

auto all_requesters() -> names_by_unit
{
  names_by_unit out;
  for(auto const& row : rm0351_requests)
  {
    auto& names = out[instance_of(row.signal)];
    if(not std::ranges::contains(names, row.signal))
    {
      names.emplace_back(row.signal);
    }
  }
  return out;
}

auto parked(core::machine_options options = popcorn::testing::stm32_options()) -> stm32_fixture
{
  stm32_fixture f{l476rc(), firmware, std::move(options)};
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  return f;
}

auto read(stm32_fixture const& f, address at) -> std::uint32_t
{
  auto const value = f.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return static_cast<std::uint32_t>(*value);
}

void write(stm32_fixture const& f, address at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value).has_value());
}

auto now_ps(stm32_fixture const& f) -> std::uint64_t
{
  return f.target->virtual_clock().time().ps;
}

void idle_to(stm32_fixture const& f, std::uint64_t ps)
{
  f.set("pc", popcorn::testing::sym(firmware, "idle"));
  f.run_to_stop({.max_instructions = icount{10'000}, .until_time = popcorn::vtime{ps}});
}

void program(stm32_fixture const& f, address dma, std::uint32_t ch, std::uint32_t items)
{
  write(f, dma + ccr(ch), 0);
  write(f, dma + 0x10 + 20 * (ch - 1), source);
  write(f, dma + 0x14 + 20 * (ch - 1), target);
  write(f, dma + cndtr(ch), items);
  write(f, dma + ccr(ch), word_sizes | en);
}

void select(stm32_fixture const& f, address dma, std::uint32_t ch, std::uint32_t code)
{
  auto const shift = 4 * (ch - 1);
  write(f, dma + cselr, (read(f, dma + cselr) & ~(0xFu << shift)) | (code << shift));
}

auto peer(stm32_fixture const& f, std::string_view unit) -> requester&
{
  auto* const found = f.model_of<requester>(unit);
  REQUIRE(found != nullptr);
  return *found;
}

auto cpu_store(stm32_fixture const& f, std::uint16_t opcode, address at, std::uint32_t value)
    -> popcorn::stop
{
  constexpr address code = 0x2000'0100;
  REQUIRE(f.target->mem().write(code, 2, opcode).has_value());
  REQUIRE(f.target->mem().write(code + 2, 2, 0xbe00).has_value());
  f.set("r0", at);
  f.set("r1", value);
  f.set("pc", code | 1u);
  return f.run_to_stop({.max_instructions = icount{4}});
}

constexpr std::uint16_t strb = 0x7001;
constexpr std::uint16_t strh = 0x8001;
}

TEST_CASE(
    "RM0351 Tables 46 and 47: the DMA1 and DMA2 variants hold every request, channel and code")
{
  auto const listed = [](stm32::dma_l1l4_variant const& variant, std::string_view instance) {
    std::vector<std::tuple<std::string_view, std::uint32_t, std::uint32_t>> out;
    for(auto const& request : variant.requests)
    {
      out.emplace_back(request.signal, request.channel, request.code);
    }
    std::vector<std::tuple<std::string_view, std::uint32_t, std::uint32_t>> expected;
    for(auto const& row : rm0351_requests)
    {
      if(row.instance == instance)
      {
        expected.emplace_back(row.signal, row.channel, row.code);
      }
    }
    std::ranges::sort(out);
    std::ranges::sort(expected);
    CHECK(out == expected);
  };
  listed(stm32::l4_dma1, "DMA1");
  listed(stm32::l4_dma2, "DMA2");
  CHECK(stm32::l4_dma1.requests.size() == 63);
  CHECK(stm32::l4_dma2.requests.size() == 49);
  CHECK(stm32::l4_dma1.selection_register == "CSELR");
  CHECK(stm32::l1_dma1.selection_register.empty());
  CHECK(stm32::l1_dma2.selection_register.empty());
}

TEST_CASE(
    "RM0351 11.6.7: every request moves one item only while CSELR selects its code, the other "
    "codes of its channel move nothing")
{
  auto const                    f = parked(with_requesters(all_requesters()));
  std::vector<std::string_view> unrouted;
  std::size_t                   routed = 0;
  for(auto const& row : rm0351_requests)
  {
    auto const unit = instance_of(row.signal);
    if(not has_unit(l476rc(), unit))
    {
      unrouted.push_back(row.signal);
      continue;
    }
    CAPTURE(row.instance);
    CAPTURE(row.channel);
    CAPTURE(row.signal);
    auto const dma = row.instance == "DMA1" ? dma1 : dma2;
    auto&      p   = peer(f, unit);
    write(f, source, 0x5A00'0000u + row.channel);
    write(f, target, 0);
    write(f, dma + ifcr, 0x0FFF'FFFFu);
    select(f, dma, row.channel, (row.code + 1) % 16);
    program(f, dma, row.channel, 1);
    p.acks.clear();
    p.set(row.signal, true);
    idle_to(f, now_ps(f) + 3 * latency_ps);
    CHECK(read(f, dma + cndtr(row.channel)) == 1);
    CHECK(p.acks.empty());
    select(f, dma, row.channel, row.code);
    idle_to(f, now_ps(f) + 3 * latency_ps);
    CHECK(read(f, dma + cndtr(row.channel)) == 0);
    CHECK(read(f, target) == 0x5A00'0000u + row.channel);
    CHECK(p.acks == std::vector<std::string>{std::string{row.signal}});
    p.set(row.signal, false);
    write(f, dma + ccr(row.channel), 0);
    write(f, dma + cselr, 0);
    ++routed;
  }
  CHECK(routed == rm0351_requests.size() - 4);
  CHECK(
      unrouted
      == std::vector<std::string_view>{"aes_in_dma", "aes_out_dma", "aes_out_dma", "aes_in_dma"});
}

TEST_CASE(
    "RM0351 Table 47: SAI1_A on DMA2 channels 1 and 6 reaches both channels, each "
    "acknowledged with the signal's request")
{
  auto const f  = parked(with_requesters({
      {"SAI1", {"sai1_a_dma"}}
  }));
  auto&      p  = peer(f, "SAI1");
  p.drop_on_ack = false;
  write(f, source, 0x1234'5678u);
  select(f, dma2, 1, 1);
  select(f, dma2, 6, 1);
  program(f, dma2, 1, 1);
  program(f, dma2, 6, 1);
  p.set("sai1_a_dma", true);
  idle_to(f, now_ps(f) + 4 * latency_ps);
  CHECK(read(f, dma2 + cndtr(1)) == 0);
  CHECK(read(f, dma2 + cndtr(6)) == 0);
  CHECK(p.acks == std::vector<std::string>{"sai1_a_dma", "sai1_a_dma"});
  p.set("sai1_a_dma", false);
  select(f, dma2, 6, 2);
  program(f, dma2, 1, 1);
  program(f, dma2, 6, 1);
  p.set("sai1_a_dma", true);
  idle_to(f, now_ps(f) + 4 * latency_ps);
  CHECK(read(f, dma2 + cndtr(1)) == 0);
  CHECK(read(f, dma2 + cndtr(6)) == 1);
}

TEST_CASE("a held request with a number above 47, USART2_TX at 55, survives a snapshot restore")
{
  auto const f = parked(with_requesters({
      {"USART2", {"usart2_tx_dma"}}
  }));
  REQUIRE(stm32::l4_dma1.requests[55].signal == "usart2_tx_dma");
  auto& p       = peer(f, "USART2");
  p.drop_on_ack = false;
  write(f, source, 0x0BAD'F00Du);
  p.set("usart2_tx_dma", true);
  auto const saved = f.target->snapshot();
  f.target->reset(reset_kind::power_on);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  f.target->restore(saved);
  select(f, dma1, 7, 2);
  program(f, dma1, 7, 1);
  idle_to(f, now_ps(f) + 3 * latency_ps);
  CHECK(read(f, dma1 + cndtr(7)) == 0);
  CHECK(read(f, target) == 0x0BAD'F00Du);
}

TEST_CASE(
    "RM0351 11.6: a firmware byte or halfword write to a DMA register warns once, raises under "
    "strict before the store, a debugger write passes, popcorn policy")
{
  {
    auto const f = parked();
    popcorn::testing::enable_clocks(
        *f.target, {
                       {"AHB1ENR", "DMA1EN"}
    });
    std::vector<core::log_record> logged;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    REQUIRE(cpu_store(f, strb, dma1 + cselr, 0x2).reason == stop_reason::user);
    CHECK(read(f, dma1 + cselr) == 0x2u);
    REQUIRE(cpu_store(f, strh, dma1 + cndtr(1), 0x10).reason == stop_reason::user);
    REQUIRE(f.target->mem().write(dma1 + cselr, 1, 0x3).has_value());
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].kind == core::log_kind::model_warning);
    CHECK(logged[0].reg == "CSELR");
    CHECK(read(f, dma1 + cndtr(1)) == 0x10u);
  }
  auto options               = popcorn::testing::stm32_options();
  options.strict_peripherals = true;
  auto const f               = parked(std::move(options));
  popcorn::testing::enable_clocks(
      *f.target, {
                     {"AHB1ENR", "DMA2EN"}
  });
  CHECK(f.target->mem().write(dma2 + cselr, 1, 0x1).has_value());
  CHECK(read(f, dma2 + cselr) == 0x1u);
  CHECK_THROWS_AS((void)cpu_store(f, strb, dma2 + cselr, 0x2), popcorn::model_error);
  CHECK(read(f, dma2 + cselr) == 0x1u);
  REQUIRE(cpu_store(f, 0x6001, dma2 + cselr, 0x2).reason == stop_reason::user);
  CHECK(read(f, dma2 + cselr) == 0x2u);
}

TEST_CASE(
    "the LL sequence copies memory to memory on DMA1 channel 1 and serves USART2_TX on "
    "channel 7 at code 2")
{
  stm32_fixture f{
      l476rc(), firmware, with_requesters({{"USART2", {"usart2_tx_dma"}}}
        )
  };
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  REQUIRE(f.log() == std::vector<std::uint32_t>{0x7, 0, 1, 1, 0x0200'0000u});
  auto& p       = peer(f, "USART2");
  p.drop_on_ack = false;
  p.set("usart2_tx_dma", true);
  REQUIRE(f.run_to_stop({.max_instructions = icount{100'000}}).reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0x7, 0, 1, 1, 0x0200'0000u, 0, 1});
  CHECK(p.acks.size() == 4);
}

TEST_CASE("the L4 DMA rows serve DMA1 and DMA2, an L1 variant does not attach to the L4 layout")
{
  auto const f = parked();
  CHECK(f.model_of<stm32::dma_controller>("DMA1") != nullptr);
  CHECK(f.model_of<stm32::dma_controller>("DMA2") != nullptr);
  auto options = popcorn::testing::stm32_options();
  options.models.insert_or_assign("DMA2", stm32::dma_l1l4_factory(stm32::l1_dma2));
  CHECK_THROWS_AS((stm32_fixture{l476rc(), firmware, std::move(options)}), popcorn::config_error);
}

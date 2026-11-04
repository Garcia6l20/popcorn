#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/audio.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::l476rc;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

constexpr std::string_view firmware = "l476_l4_rcc_ll";
constexpr std::uint64_t    us       = 1'000'000;
constexpr std::uint64_t    kernel   = 31'250;
constexpr std::uint32_t    sai1_irq = 74;

class recorder final : public stm32::audio_endpoint
{
public:
  explicit recorder(std::vector<std::uint32_t> words = {}) : words_{std::move(words)}
  {}

  std::vector<std::uint64_t> starts;
  std::vector<std::uint64_t> ends;
  std::vector<std::uint64_t> received;

  void
      transmitted(std::span<std::uint32_t const>, stm32::audio_format, vtime at, vtime end) override
  {
    starts.push_back(at.ps);
    ends.push_back(end.ps);
  }

  void receive(std::span<std::uint32_t> slots, stm32::audio_format, vtime at) override
  {
    received.push_back(at.ps);
    for(auto& slot : slots)
    {
      slot = next_ < words_.size() ? words_[next_++] : 0;
    }
  }

private:
  std::vector<std::uint32_t> words_;
  std::size_t                next_{};
};

using fields = std::vector<std::pair<std::string_view, std::uint32_t>>;

struct rig
{
  std::shared_ptr<recorder>      a = std::make_shared<recorder>();
  std::shared_ptr<recorder>      b = std::make_shared<recorder>(std::vector<std::uint32_t>(64, 7));
  std::unique_ptr<stm32_fixture> f;

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  rig()
  {
    auto board            = stm32::board{};
    board.audio["SAI1_A"] = {.endpoint = a};
    board.audio["SAI1_B"] = {.endpoint = b};
    auto options          = popcorn::testing::stm32_options();
    options.models        = stm32::factories(board, l476rc());
    f                     = std::make_unique<stm32_fixture>(l476rc(), firmware, std::move(options));
    f->set("pc", sym(firmware, "idle"));
  }

  [[nodiscard]] auto reg(std::string_view unit, std::string_view name) const
      -> popcorn::register_desc const&
  {
    auto const* const found =
        popcorn::find_register(f->target->peripheral(unit).description(), name);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto at(std::string_view unit, std::string_view name) const -> popcorn::address
  {
    return f->target->peripheral(unit).description().base + reg(unit, name).offset;
  }

  [[nodiscard]] auto read(popcorn::address address) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(address, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(popcorn::address address, std::uint32_t value) const
  {
    REQUIRE(f->target->mem().write(address, 4, value).has_value());
  }

  void put(std::string_view name, fields const& values) const
  {
    auto const&   described = reg("SAI1", name);
    std::uint32_t word      = 0;
    for(auto const& [field, bits] : values)
    {
      auto const found = std::ranges::find(described.fields, field, &popcorn::field_desc::name);
      REQUIRE(found != described.fields.end());
      word |= bits << found->lsb;
    }
    write(at("SAI1", name), word);
  }

  void
      set(std::string_view unit,
          std::string_view name,
          std::string_view field,
          std::uint32_t    value) const
  {
    auto const& described = reg(unit, name).fields;
    auto const  found     = std::ranges::find(described, field, &popcorn::field_desc::name);
    REQUIRE(found != described.end());
    auto const mask    = ((std::uint64_t{1} << found->width) - 1u) << found->lsb;
    auto const address = at(unit, name);
    write(
        address, static_cast<std::uint32_t>(
                     (read(address) & ~mask) | ((std::uint64_t{value} << found->lsb) & mask)));
  }

  [[nodiscard]] auto get(std::string_view name, std::string_view field) const -> std::uint32_t
  {
    return f->field("SAI1", name, field);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return f->target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{10'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  void clocks() const
  {
    set("RCC", "CR", "HSION", 1);
    run_until(now() + 200 * us);
    REQUIRE(f->field("RCC", "CR", "HSIRDY") == 1);
    set("RCC", "PLLCFGR", "PLLSRC", 2);
    set("RCC", "PLLCFGR", "PLLM", 1);
    set("RCC", "PLLSAI1CFGR", "PLLSAI1N", 28);
    set("RCC", "PLLSAI1CFGR", "PLLSAI1PEN", 1);
    set("RCC", "CR", "PLLSAI1ON", 1);
    run_until(now() + 200 * us);
    REQUIRE(f->field("RCC", "CR", "PLLSAI1RDY") == 1);
    set("RCC", "APB2ENR", "SAI1EN", 1);
  }

  void stereo32(std::string_view prefix) const
  {
    put(
        std::string{
            prefix
    }
            + "FRCR",
        {{"FRL", 63}, {"FSALL", 31}});
    put(
        std::string{
            prefix
    }
            + "SLOTR",
        {{"NBSLOT", 1}, {"SLOTEN", 3}});
  }

  void transmit(std::uint32_t divider, std::uint32_t no_divider) const
  {
    stereo32("A");
    put("ACR1", {
                    {"DS",     7         },
                    {"MCKDIV", divider   },
                    {"NODIV",  no_divider},
                    {"SAIEN",  1         }
    });
    for(std::uint32_t word = 0; word < 8; ++word)
    {
      write(at("SAI1", "ADR"), word);
    }
  }

  [[nodiscard]] auto period() const -> std::uint64_t
  {
    REQUIRE(a->starts.size() >= 2);
    CHECK(a->ends[0] - a->starts[0] == a->starts[1] - a->starts[0]);
    return a->starts[1] - a->starts[0];
  }
};
}

TEST_CASE("RM0351 43.3.8: MCKDIV above 0 divides by 2 x MCKDIV, 0 by 1, frame of 256 MCLK periods")
{
  for(auto const [divider, ratio] : std::to_array<std::pair<std::uint32_t, std::uint64_t>>({
          {0,  1 },
          {1,  2 },
          {2,  4 },
          {3,  6 },
          {15, 30}
  }))
  {
    CAPTURE(divider);
    rig r;
    r.clocks();
    r.transmit(divider, 0);
    r.run_until(r.now() + 3 * 256 * ratio * kernel);
    CHECK(r.period() == 256 * ratio * kernel);
  }
}

TEST_CASE("RM0351 43.5.2: NODIV 1 bypasses MCKDIV, a frame lasts FRL + 1 kernel clocks")
{
  rig r;
  r.clocks();
  r.transmit(3, 1);
  r.run_until(r.now() + 4 * us);
  CHECK(r.period() == 64 * kernel);
}

TEST_CASE("RM0351 43.3.9: FLVL and FREQ follow the FIFO level and FTH, FFLUSH empties it")
{
  rig r;
  CHECK(r.get("ASR", "FREQ") == 1);
  constexpr std::array<std::uint32_t, 9> levels{0, 1, 1, 2, 2, 3, 3, 4, 5};
  for(std::uint32_t threshold = 0; threshold <= 4; ++threshold)
  {
    r.put(
        "ACR2", {
                    {"FTH",    threshold},
                    {"FFLUSH", 1        }
    });
    CHECK(r.get("ACR2", "FFLUSH") == 0);
    for(std::uint32_t count = 0; count <= 9; ++count)
    {
      CAPTURE(threshold);
      CAPTURE(count);
      auto const level = levels[std::min<std::uint32_t>(count, 8)];
      CHECK(r.get("ASR", "FLVL") == level);
      CHECK(r.get("ASR", "FREQ") == (level <= threshold ? 1u : 0u));
      r.write(r.at("SAI1", "ADR"), count);
    }
  }
}

TEST_CASE("RM0351 43.3.3: block B synchronous with block A receives in A's frames")
{
  rig r;
  r.clocks();
  r.stereo32("B");
  r.put(
      "BCR1", {
                  {"MODE",   3},
                  {"DS",     7},
                  {"SYNCEN", 1},
                  {"SAIEN",  1}
  });
  r.transmit(1, 0);
  r.run_until(r.now() + 4 * 512 * kernel);
  REQUIRE(r.a->starts.size() >= 3);
  REQUIRE(r.b->received.size() >= 3);
  CHECK(
      std::vector(r.b->received.begin(), r.b->received.begin() + 3)
      == std::vector(r.a->starts.begin(), r.a->starts.begin() + 3));
  CHECK(r.get("BSR", "FLVL") >= 2);
}

TEST_CASE("RM0351 43.5.6 and 43.5.8: WCKCFGIE raises SAI1_IRQn, CWCKCFG clears WCKCFG")
{
  rig r;
  r.clocks();
  r.put(
      "AFRCR", {
                   {"FRL",   23},
                   {"FSALL", 11}
  });
  r.put(
      "AIM", {
                 {"WCKCFGIE", 1}
  });
  r.put(
      "ACR1", {
                  {"MODE",  1},
                  {"DS",    4},
                  {"SAIEN", 1}
  });
  CHECK(r.get("ACR1", "SAIEN") == 0);
  CHECK(r.get("ASR", "WCKCFG") == 1);
  CHECK(r.f->pending(sai1_irq));
  r.put(
      "ACLRFR", {
                    {"CWCKCFG", 1}
  });
  CHECK(r.get("ASR", "WCKCFG") == 0);
}

TEST_CASE("RM0351 43.5.6 and 43.5.8: an underrun sets OVRUDR, OVRUDRIE raises, COVRUDR clears")
{
  rig r;
  r.clocks();
  r.put(
      "AIM", {
                 {"OVRUDRIE", 1}
  });
  r.transmit(0, 0);
  r.run_until(r.now() + 6 * 256 * kernel);
  CHECK(r.get("ASR", "OVRUDR") == 1);
  CHECK(r.f->pending(sai1_irq));
  r.put(
      "ACLRFR", {
                    {"COVRUDR", 1}
  });
  CHECK(r.get("ASR", "OVRUDR") == 0);
}

TEST_CASE("RM0351 43.5: SAI1 and SAI2 have a model with no PDMCR or PDMDLY word")
{
  rig r;
  for(auto const* unit : {"SAI1", "SAI2"})
  {
    CAPTURE(unit);
    auto* const model = r.f->target->model(unit);
    REQUIRE(model != nullptr);
    auto const claimed = model->claims();
    CHECK(std::ranges::contains(claimed, std::string{"BDR"}));
    CHECK_FALSE(std::ranges::contains(claimed, std::string{"PDMCR"}));
    CHECK_FALSE(std::ranges::contains(claimed, std::string{"PDMDLY"}));
  }
}

TEST_CASE("RM0351 43.5: the SAI1 and SAI2 fields carry their RM0351 names")
{
  rig        r;
  auto const named = std::to_array<std::pair<std::string_view, std::vector<std::string_view>>>({
      {"ACR1",   {"SAIEN", "OUTDRIV"}                                              },
      {"BCR1",   {"SAIEN", "OUTDRIV"}                                              },
      {"ACR2",   {"FFLUSH", "MUTECNT"}                                             },
      {"BCR2",   {"FFLUSH", "MUTECNT"}                                             },
      {"AIM",    {"LFSDETIE", "MUTEDETIE", "WCKCFGIE"}                             },
      {"BIM",    {"LFSDETIE", "MUTEDETIE", "WCKCFGIE"}                             },
      {"ACLRFR", {"CLFSDET", "CAFSDET", "CCNRDY", "CWCKCFG", "CMUTEDET", "COVRUDR"}},
      {"BCLRFR", {"CLFSDET", "CAFSDET", "CCNRDY", "CWCKCFG", "CMUTEDET", "COVRUDR"}},
  });
  for(auto const* unit : {"SAI1", "SAI2"})
  {
    for(auto const& [name, wanted] : named)
    {
      auto const& fields = r.reg(unit, name).fields;
      for(auto const field : wanted)
      {
        CAPTURE(unit);
        CAPTURE(name);
        CAPTURE(field);
        CHECK(std::ranges::contains(fields, field, &popcorn::field_desc::name));
      }
    }
  }
}

TEST_CASE("RM0351 43.3.8, 43.3.15 and 11.4.5: LL I2S master TX at 47.991 kHz on a DMA2 ring")
{
  constexpr std::string_view sai_ll = "l476_l4_sai_ll";
  constexpr std::uint64_t    hz     = 16'000'000 / 2 * 43 / 7;
  constexpr std::uint64_t    frame  = 256 * 4 * 1'000'000'000'000;
  constexpr std::size_t      frames = 4 * 64 / 2;
  auto const                 sink   = std::make_shared<stm32::audio_capture_endpoint>();
  auto                       board  = stm32::board{};
  board.audio["SAI1_A"]             = {.endpoint = sink};
  auto options                      = popcorn::testing::stm32_options();
  options.models                    = stm32::factories(board, l476rc());
  stm32_fixture f{l476rc(), sai_ll, std::move(options)};
  REQUIRE(
      f.run_to_stop({.max_instructions = icount{1'000'000}, .until = sym(sai_ll, "main")}).reason
      == stop_reason::address);
  f.set("pc", sym(sai_ll, "tx_dma"));
  auto const stopped = f.run_to_stop({.max_instructions = icount{1'000'000}});
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{0x0001'0000, 4, 4, 0, 0x1});
  REQUIRE(sink->frames.size() >= frames);
  auto const word = [](std::uint32_t index) {
    return 0x5a00'0000u | ((index % 64u) << 8) | (index % 64u);
  };
  for(std::uint32_t index = 0; index < frames; ++index)
  {
    CAPTURE(index);
    CHECK(
        sink->frames[index].slots
        == std::vector<std::uint32_t>{word(2 * index), word(2 * index + 1)});
    auto const gap   = sink->frames[index].at.ps - sink->frames[0].at.ps;
    auto const exact = index * frame / hz;
    CHECK(gap >= exact);
    CHECK(gap <= exact + 1);
  }
}

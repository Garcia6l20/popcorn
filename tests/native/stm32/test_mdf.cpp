#include "dma.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/pdm.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::uint64_t us           = 1'000'000;
constexpr std::uint32_t mdf_irq      = 102;
constexpr std::uint32_t adf_irq      = 112;
constexpr std::uint32_t sec_offset   = 0x1000'0000;
constexpr std::uint32_t flt0_request = 92;

class request_probe final : public popcorn::periph::model, public stm32::dma_controller
{
public:
  std::map<std::uint32_t, std::vector<bool>> levels;

  void on_request(std::uint32_t request, bool level) override
  {
    auto& seen = levels[request];
    if(seen.empty() or seen.back() != level)
    {
      seen.push_back(level);
    }
  }

  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override
  {
    if(signal == "mdf1_flt0_dma")
    {
      return flt0_request;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

auto options_for(bool strict, bool probe = false) -> core::machine_options
{
  stm32::board board{};
  auto         options       = popcorn::testing::stm32_options();
  options.models             = stm32::factories(board, u575());
  options.strict_peripherals = strict;
  if(probe)
  {
    options.models.insert_or_assign(
        "GPDMA1",
        [](popcorn::periph::model_context&,
           popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
          return std::make_shared<request_probe>();
        });
  }
  return options;
}

struct rig : stm32_fixture
{
  std::vector<core::log_record> logged;
  core::hook_handle             sink;

  explicit rig(bool strict = false, bool probe = false)
      : stm32_fixture{u575(), "rcc_ll", options_for(strict, probe)},
        sink{target->add_log_sink(
            [this](core::log_record const& record) { logged.push_back(record); })}
  {
    set("pc", sym("rcc_ll", "idle"));
    put("RCC", "RCC_AHB1ENR", "MDF1EN", 1);
    put("RCC", "RCC_AHB3ENR", "ADF1EN", 1);
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> popcorn::address
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(target->mem().write(at(peripheral, reg), 4, value).has_value());
  }

  void write(std::string_view reg, std::uint32_t value) const
  {
    write("MDF1", reg, value);
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const value = target->mem().read(at(peripheral, reg), 4);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto read(std::string_view reg) const -> std::uint32_t
  {
    return read("MDF1", reg);
  }

  [[nodiscard]] auto bits(std::string_view reg, std::string_view name) const -> std::uint32_t
  {
    return field("MDF1", reg, name);
  }

  [[nodiscard]] auto shifted(
      std::string_view peripheral,
      std::string_view reg,
      std::string_view name,
      std::uint32_t    value) const -> std::uint32_t
  {
    auto const& desc  = target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = ((std::uint64_t{1} << bits->width) - 1u) << bits->lsb;
    return static_cast<std::uint32_t>((std::uint64_t{value} << bits->lsb) & mask);
  }

  void
      put(std::string_view peripheral,
          std::string_view reg,
          std::string_view name,
          std::uint32_t    value) const
  {
    auto const mask = shifted(peripheral, reg, name, ~std::uint32_t{});
    write(peripheral, reg, (read(peripheral, reg) & ~mask) | shifted(peripheral, reg, name, value));
  }

  void put(std::string_view reg, std::string_view name, std::uint32_t value) const
  {
    put("MDF1", reg, name, value);
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return target->virtual_clock().time().ps;
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        run_to_stop({.max_instructions = icount{100'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto pending(std::uint32_t irq) const -> bool
  {
    auto const word = target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }

  [[nodiscard]] auto warnings() const -> std::vector<core::log_record>
  {
    std::vector<core::log_record> out;
    for(auto const& record : logged)
    {
      if(record.kind == core::log_kind::model_warning)
      {
        out.push_back(record);
      }
    }
    return out;
  }

  void pll3_kernel(std::uint32_t m, std::uint32_t n, std::uint32_t q) const
  {
    put("RCC", "RCC_CR", "HSION", 1);
    run_until(now() + 200 * us);
    write("RCC", "RCC_PLL3CFGR", (2u << 0) | (3u << 2) | ((m - 1) << 8) | (1u << 17));
    write("RCC", "RCC_PLL3DIVR", (n - 1) | ((q - 1) << 16));
    put("RCC", "RCC_CR", "PLL3ON", 1);
    run_until(now() + 200 * us);
    REQUIRE(field("RCC", "RCC_CR", "PLL3RDY") == 1);
    put("RCC", "RCC_CCIPR2", "MDF1SEL", 2);
  }

  [[nodiscard]] auto warned(std::string_view text) const -> std::size_t
  {
    std::size_t count = 0;
    for(auto const& record : warnings())
    {
      count += record.message.contains(text) ? 1 : 0;
    }
    return count;
  }

  void clocks(std::uint32_t procdiv, std::uint32_t cckdiv) const
  {
    write("CKGCR", (procdiv << 24) | (cckdiv << 16) | (1u << 5) | (1u << 1));
    put("CKGCR", "CKGDEN", 1);
  }
};

auto claimed(rig const& r, std::string_view peripheral) -> std::vector<std::string>
{
  auto const* model = r.target->model(peripheral);
  REQUIRE(model != nullptr);
  return model->claims();
}

template <typename F>
void check_unmodelled(F&& call, std::string_view reg, std::string_view field, std::uint32_t value)
{
  try
  {
    call();
    FAIL("no unmodelled_config");
  }
  catch(popcorn::unmodelled_config const& error)
  {
    CHECK(error.reg() == reg);
    CHECK(error.field() == field);
    CHECK(error.value() == value);
  }
}
}

TEST_CASE("MDF1 claims its 98 registers, ADF1 its 15, the secure aliases reach the same model")
{
  rig const r;
  CHECK(claimed(r, "MDF1").size() == 98);
  CHECK(claimed(r, "ADF1").size() == 15);
  REQUIRE(r.target->mem().write(r.at("MDF1", "SITF0CR") + sec_offset, 4, 0x1F01).has_value());
  CHECK(r.bits("SITF0CR", "SITFACTIVE") == 1);
  REQUIRE(r.target->mem().write(r.at("ADF1", "SITF0CR") + sec_offset, 4, 0x1F01).has_value());
  CHECK(r.field("ADF1", "SITF0CR", "SITFACTIVE") == 1);
}

TEST_CASE("reset values: SITFxCR 0x1F00, every other register 0")
{
  rig const r;
  for(auto const* peripheral : {"MDF1", "ADF1"})
  {
    for(auto const& name : claimed(r, peripheral))
    {
      CAPTURE(peripheral);
      CAPTURE(name);
      CHECK(r.read(peripheral, name) == (name.starts_with("SITF") ? 0x1F00u : 0u));
    }
  }
}

TEST_CASE("ACTIVE flags follow their enables, BSMXACTIVE follows DFLTACTIVE, DFLTEN reads 0")
{
  rig const r;
  r.put("CKGCR", "CKGDEN", 1);
  CHECK(r.bits("CKGCR", "CKGACTIVE") == 1);
  r.write("CKGCR", 1u << 31);
  CHECK(r.bits("CKGCR", "CKGACTIVE") == 0);
  r.write("DFLT2CR", 1);
  CHECK(r.bits("DFLT2CR", "DFLTACTIVE") == 1);
  CHECK(r.bits("BSMX2CR", "BSMXACTIVE") == 1);
  CHECK(r.bits("DFLT2CR", "DFLTRUN") == 1);
  CHECK((r.read("DFLT2CR") & 1u) == 0);
  r.write("DFLT2CR", 0);
  CHECK(r.bits("DFLT2CR", "DFLTACTIVE") == 0);
  CHECK(r.bits("BSMX2CR", "BSMXACTIVE") == 0);
  r.write("BSMX2CR", 1u << 31);
  CHECK(r.bits("BSMX2CR", "BSMXACTIVE") == 0);
  r.write("ADF1", "SADCR", 1);
  CHECK(r.field("ADF1", "SADCR", "SADACTIVE") == 1);
  r.write("ADF1", "DFLT0CICR", 5u << 4);
  r.write("ADF1", "DFLT0CR", 1);
  CHECK(r.field("ADF1", "DFLT0CR", "DFLTACTIVE") == 1);
  CHECK(r.field("ADF1", "BSMX0CR", "BSMXACTIVE") == 1);
  r.write("ADF1", "SITF0CR", 0x1F00u | (1u << 31));
  CHECK(r.field("ADF1", "SITF0CR", "SITFACTIVE") == 0);
  r.write("GCR", 1);
  CHECK(r.bits("GCR", "TRGO") == 0);
}

namespace
{
struct protected_field
{
  std::string_view enable_reg;
  std::uint32_t    enable;
  std::string_view reg;
  std::string_view field;
};

constexpr protected_field protected_fields[]{
    {"DFLT0CR", 1,      "GCR",       "ILVNB"   },
    {"CKGCR",   1,      "CKGCR",     "PROCDIV" },
    {"CKGCR",   1,      "CKGCR",     "CCKDIV"  },
    {"CKGCR",   1,      "CKGCR",     "CKGMOD"  },
    {"CKGCR",   1,      "CKGCR",     "TRGSRC"  },
    {"CKGCR",   1,      "CKGCR",     "TRGSENS" },
    {"CKGCR",   1,      "CKGCR",     "CCK0DIR" },
    {"CKGCR",   1,      "CKGCR",     "CCK1DIR" },
    {"SITF1CR", 0x1F11, "SITF1CR",   "STH"     },
    {"SITF1CR", 0x1F11, "SITF1CR",   "SITFMOD" },
    {"SITF1CR", 0x1F11, "SITF1CR",   "SCKSRC"  },
    {"DFLT1CR", 1,      "BSMX1CR",   "BSSEL"   },
    {"DFLT1CR", 1,      "DFLT1CR",   "NBDIS"   },
    {"DFLT1CR", 1,      "DFLT1CR",   "TRGSRC"  },
    {"DFLT1CR", 1,      "DFLT1CR",   "TRGSENS" },
    {"DFLT1CR", 1,      "DFLT1CR",   "FTH"     },
    {"DFLT1CR", 1,      "DFLT1CR",   "DMAEN"   },
    {"DFLT1CR", 1,      "DFLT1CR",   "SNPSFMT" },
    {"DFLT1CR", 1,      "DFLT1CR",   "ACQMOD"  },
    {"DFLT1CR", 1,      "DFLT1CICR", "MCICD"   },
    {"DFLT1CR", 1,      "DFLT1CICR", "CICMOD"  },
    {"DFLT1CR", 1,      "DFLT1CICR", "DATSRC"  },
    {"DFLT1CR", 1,      "DFLT1RSFR", "RSFLTBYP"},
    {"DFLT1CR", 1,      "DFLT1RSFR", "RSFLTD"  },
    {"DFLT1CR", 1,      "DFLT1RSFR", "HPFBYP"  },
    {"DFLT1CR", 1,      "DFLT1RSFR", "HPFC"    },
    {"DFLT1CR", 1,      "DFLT1INTR", "INTDIV"  },
    {"DFLT1CR", 1,      "DFLT1INTR", "INTVAL"  },
};
}

TEST_CASE("a protected field changed while active keeps its value, logs once, raises when strict")
{
  for(auto const& entry : protected_fields)
  {
    CAPTURE(entry.reg);
    CAPTURE(entry.field);
    {
      rig r;
      r.write("CKGCR", 3u << 16);
      r.write(entry.enable_reg, entry.enable);
      auto const keep   = entry.reg == entry.enable_reg ? entry.enable : 0u;
      auto const before = r.read(entry.reg);
      auto const mask   = r.shifted("MDF1", entry.reg, entry.field, ~0u);
      auto const other  = r.shifted("MDF1", entry.reg, entry.field, 1) == (before & mask) ? 2u : 1u;
      auto const value =
          ((before | keep) & ~mask) | r.shifted("MDF1", entry.reg, entry.field, other);
      r.write(entry.reg, value);
      CHECK(r.read(entry.reg) == before);
      r.write(entry.reg, value);
      std::vector<core::log_record> warned;
      for(auto const& record : r.warnings())
      {
        if(record.message.contains("Table 381"))
        {
          warned.push_back(record);
        }
      }
      REQUIRE(warned.size() == 1);
      CHECK(warned[0].reg == entry.reg);
    }
    rig r{true};
    r.write("CKGCR", 3u << 16);
    r.write(entry.enable_reg, entry.enable);
    auto const keep   = entry.reg == entry.enable_reg ? entry.enable : 0u;
    auto const before = r.read(entry.reg);
    auto const mask   = r.shifted("MDF1", entry.reg, entry.field, ~0u);
    auto const value  = ((before | keep) & ~mask) | (~before & mask);
    CHECK_THROWS_AS(
        static_cast<void>(r.target->mem().write(r.at("MDF1", entry.reg), 4, value)),
        popcorn::model_error);
    CHECK(r.read(entry.reg) == before);
  }
}

TEST_CASE("CKGCR |= CKGDEN after programming keeps every field and logs nothing")
{
  rig const r;
  r.clocks(4, 1);
  CHECK(r.bits("CKGCR", "PROCDIV") == 4);
  CHECK(r.bits("CKGCR", "CCKDIV") == 1);
  CHECK(r.bits("CKGCR", "CKGACTIVE") == 1);
  r.put("CKGCR", "CKGDEN", 1);
  CHECK(r.warnings().empty());
}

namespace
{
using writes = std::vector<std::pair<std::string_view, std::uint32_t>>;

struct unmodelled_row
{
  std::string_view peripheral;
  writes           setup;
  std::string_view reg;
  std::uint32_t    value;
  std::string_view error_reg;
  std::string_view field;
  std::uint32_t    field_value;
  writes           fixup;
  std::uint32_t    retry;
  std::string_view check_reg;
  std::string_view check_field;
  std::uint32_t    check_value;
};

auto unmodelled_rows() -> std::vector<unmodelled_row>
{
  constexpr std::uint32_t cck0_in  = 0x0100'0003u;
  constexpr std::uint32_t cck0_out = 0x0100'0023u;
  return {
      {"MDF1", {},                   "CKGCR",   (1u << 4) | 1u, "CKGCR",   "CKGMOD",  1, {},                                  1,    "CKGCR",   "CKGACTIVE",  1},
      {"MDF1",
       {{"CKGCR", cck0_in}},
       "SITF0CR",                               0x1F11,
       "CKGCR",                                                            "CCK0DIR",
       0,                                                                                {{"CKGCR", 0}, {"CKGCR", cck0_out}},
       0x1F11,                                                                                                                      "SITF0CR",
       "SITFACTIVE",                                                                                                                                         1},
      {"MDF1",
       {{"SITF0CR", 0x1F11}},
       "CKGCR",                                 cck0_in,
       "CKGCR",                                                            "CCK0DIR",
       0,                                                                                {},
       cck0_out,                                                                                                                    "CKGCR",
       "CKGACTIVE",                                                                                                                                          1},
      {"MDF1",
       {},
       "SITF0CR",                               0x1F21,
       "SITF0CR",                                                          "SITFMOD",
       2,                                                                                {},
       0x1F11,                                                                                                                      "SITF0CR",
       "SITFACTIVE",                                                                                                                                         1},
      {"MDF1",
       {},
       "SITF0CR",                               0x1F35,
       "SITF0CR",                                                          "SITFMOD",
       3,                                                                                {},
       0x1F11,                                                                                                                      "SITF0CR",
       "SITFACTIVE",                                                                                                                                         1},
      {"MDF1",
       {},
       "SITF0CR",                               0x1F15,
       "SITF0CR",                                                          "SCKSRC",
       2,                                                                                {},
       0x1F13,                                                                                                                      "SITF0CR",
       "SITFACTIVE",                                                                                                                                         1},
      {"MDF1",
       {{"DFLT0CICR", 2}},
       "DFLT0CR",                               1,
       "DFLT0CICR",                                                        "DATSRC",
       2,                                                                                {{"DFLT0CICR", 0}},
       1,                                                                                                                           "DFLT0CR",
       "DFLTACTIVE",                                                                                                                                         1},
      {"MDF1", {},                   "DFLT0CR", 0x41,           "DFLT0CR", "ACQMOD",  4, {},                                  0x61, "DFLT0CR", "DFLTACTIVE", 1},
      {"MDF1", {},                   "DFLT0CR", 0x51,           "DFLT0CR", "ACQMOD",  5, {},                                  0x01, "DFLT0CR", "DFLTACTIVE", 1},
      {"MDF1",
       {},
       "DFLT0CR",                               0x2021,
       "DFLT0CR",                                                          "TRGSRC",
       2,                                                                                {},
       0x0021,                                                                                                                      "DFLT0CR",
       "DFLTACTIVE",                                                                                                                                         1},
      {"MDF1",
       {},
       "DFLT0CR",                               0x1031,
       "DFLT0CR",                                                          "TRGSRC",
       1,                                                                                {},
       0x0031,                                                                                                                      "DFLT0CR",
       "DFLTACTIVE",                                                                                                                                         1},
      {"MDF1",
       {{"DFLT0CICR", 0x19u << 20}},
       "DFLT0CR",                               1,
       "DFLT0CICR",                                                        "SCALE",
       0x19,                                                                             {{"DFLT0CICR", 0}},
       1,                                                                                                                           "DFLT0CR",
       "DFLTACTIVE",                                                                                                                                         1},
      {"ADF1",
       {},
       "DFLT0CR",                               1,
       "DFLT0CICR",                                                        "CICMOD",
       0,                                                                                {{"DFLT0CICR", 5u << 4}},
       1,                                                                                                                           "DFLT0CR",
       "DFLTACTIVE",                                                                                                                                         1},
      {"ADF1",
       {{"DFLT0CICR", 3u << 4}},
       "DFLT0CR",                               1,
       "DFLT0CICR",                                                        "CICMOD",
       3,                                                                                {{"DFLT0CICR", 4u << 4}},
       1,                                                                                                                           "DFLT0CR",
       "DFLTACTIVE",                                                                                                                                         1},
      {"MDF1",
       {{"DFLT0CR", 1}},
       "DFLT0CICR",                             0x30u << 20,
       "DFLT0CICR",                                                        "SCALE",
       0x30,                                                                             {},
       0x2Cu << 20,
       "DFLT0CICR",                                                                                                                            "SCALE",
       0x2C                                                                                                                                                   },
      {"MDF1", {},                   "OLD0CR",  1,              "OLD0CR",  "OLDEN",   1, {},                                  0x2,  "OLD0CR",  "THINB",      1},
      {"MDF1", {},                   "SCD0CR",  1,              "SCD0CR",  "SCDEN",   1, {},                                  0x10, "SCD0CR",  "BKSCD",      1},
      {"MDF1",
       {{"GCR", 6u << 4}},
       "DFLT0CR",                               1,
       "GCR",                                                              "ILVNB",
       6,                                                                                {{"GCR", 5u << 4}},
       1,                                                                                                                           "DFLT0CR",
       "DFLTACTIVE",                                                                                                                                         1},
  };
}
}

TEST_CASE("each unmodelled setting raises naming its field, restores the register, a retry works")
{
  for(auto const& row : unmodelled_rows())
  {
    CAPTURE(row.peripheral);
    CAPTURE(row.reg);
    CAPTURE(row.field);
    CAPTURE(row.field_value);
    rig const r;
    for(auto const& [reg, value] : row.setup)
    {
      r.write(row.peripheral, reg, value);
    }
    auto const before = r.read(row.peripheral, row.reg);
    check_unmodelled(
        [&] {
          static_cast<void>(r.target->mem().write(r.at(row.peripheral, row.reg), 4, row.value));
        },
        row.error_reg, row.field, row.field_value);
    CHECK(r.read(row.peripheral, row.reg) == before);
    for(auto const& [reg, value] : row.fixup)
    {
      r.write(row.peripheral, reg, value);
    }
    r.write(row.peripheral, row.reg, row.retry);
    CHECK(r.field(row.peripheral, row.check_reg, row.check_field) == row.check_value);
  }
}

TEST_CASE("ACQMOD 110 and MDF CICMOD 111 enable without a raise")
{
  rig const r;
  r.write("DFLT0CICR", 7u << 4);
  r.write("DFLT0CR", (6u << 4) | 1u);
  CHECK(r.bits("DFLT0CR", "DFLTACTIVE") == 1);
  CHECK(r.warnings().empty());
}

namespace
{
void peer_sequence(rig const& r, std::uint32_t sitfmod)
{
  r.write("GCR", 0);
  r.put("GCR", "ILVNB", 1);
  r.write("CKGCR", 0);
  r.write("CKGCR", (4u << 24) | (1u << 16) | (1u << 12) | (1u << 5) | (1u << 1));
  r.put("CKGCR", "CKGDEN", 1);
  for(auto const s : {0, 1})
  {
    auto const cr = std::format("SITF{}CR", s);
    r.write(cr, 0);
    r.write(cr, (0x1Fu << 8) | (sitfmod << 4));
  }
  for(auto const f : {0, 1})
  {
    for(auto const* name :
        {"BSMX{}CR", "DFLT{}CR", "DFLT{}CICR", "DFLT{}RSFR", "DFLT{}INTR", "OLD{}CR", "DLY{}CR",
         "SCD{}CR", "DFLT{}IER"})
    {
      r.write(std::vformat(name, std::make_format_args(f)), 0);
    }
    r.write(std::format("DFLT{}ISR", f), 0xFFFF'FFFF);
    r.write(std::format("BSMX{}CR", f), f == 0 ? 1 : 2);
    r.write(std::format("DFLT{}CR", f), (2u << 4) | (1u << 1));
    r.write(std::format("DFLT{}CICR", f), (0x2Cu << 20) | (31u << 8) | (5u << 4));
    r.write(std::format("DFLT{}RSFR", f), 0);
    r.put(std::format("DFLT{}RSFR", f), "HPFC", 2);
  }
  for(int round = 0; round < 2; ++round)
  {
    for(auto const f : {0, 1})
    {
      r.put(std::format("SITF{}CR", f), "SITFEN", 1);
      r.put(std::format("DFLT{}CR", f), "DFLTEN", 1);
    }
    r.put("GCR", "TRGO", 1);
    for(auto const f : {0, 1})
    {
      r.put(std::format("SITF{}CR", f), "SITFEN", 0);
      r.write(std::format("DFLT{}CR", f), (2u << 4) | (1u << 1));
    }
  }
}
}

TEST_CASE(
    "the peer's write sequence logs one Table 374 warning naming its fields, none in LF_MASTER")
{
  {
    rig const r;
    peer_sequence(r, 1);
    auto const warned = r.warnings();
    REQUIRE(warned.size() == 1);
    CHECK(warned[0].peripheral == "MDF1");
    CHECK(warned[0].message.contains("PROCDIV"));
    CHECK(warned[0].message.contains("CCKDIV"));
    CHECK(warned[0].message.contains("SITFMOD"));
    CHECK(warned[0].message.contains("Table 374"));
    CHECK(r.bits("CKGCR", "PROCDIV") == 4);
  }
  rig const r;
  peer_sequence(r, 0);
  CHECK(r.warnings().empty());
}

TEST_CASE("under strict the Table 374 break raises, SITF0CR keeps its value, a valid write works")
{
  rig const r{true};
  r.clocks(4, 1);
  r.write("SITF0CR", 0x1F10);
  CHECK_THROWS_AS(
      static_cast<void>(r.target->mem().write(r.at("MDF1", "SITF0CR"), 4, 0x1F11)),
      popcorn::model_error);
  CHECK(r.read("SITF0CR") == 0x1F10);
  r.write("SITF0CR", 0x1F01);
  CHECK(r.bits("SITF0CR", "SITFACTIVE") == 1);
}

TEST_CASE(
    "Table 374 boundaries: LF_MASTER at 2 x and normal SPI at 4 x pass, normal SPI at 2 x logs")
{
  {
    rig const r;
    r.clocks(0, 1);
    r.write("SITF0CR", 0x1F01);
    CHECK(r.warnings().empty());
  }
  {
    rig const r;
    r.clocks(0, 3);
    r.write("SITF0CR", 0x1F11);
    CHECK(r.warnings().empty());
  }
  {
    rig const r;
    r.clocks(0, 2);
    r.write("SITF0CR", 0x1F11);
    CHECK(r.warnings().size() == 1);
  }
  rig const r;
  r.clocks(0, 1);
  r.write("SITF0CR", 0x1F11);
  CHECK(r.warnings().size() == 1);
}

TEST_CASE("RSFLT at a low decimation breaks Table 374 at DFLTEN, STH below 4 at SITFEN")
{
  {
    rig const r;
    r.clocks(0, 3);
    r.write("SITF0CR", 0x1F11);
    r.write("DFLT0CICR", 3u << 8);
    r.write("DFLT0CR", 1);
    auto const warned = r.warnings();
    REQUIRE(warned.size() == 1);
    CHECK(warned[0].reg == "DFLT0CR");
    CHECK(warned[0].message.contains("RSFLT"));
  }
  rig const r;
  r.clocks(0, 3);
  r.write("SITF0CR", 0x0311);
  auto const warned = r.warnings();
  REQUIRE(warned.size() == 1);
  CHECK(warned[0].message.contains("STH 3"));
}

TEST_CASE("SKPDLY 0 written leaves SKPBF 0")
{
  rig const r;
  r.write("DLY0CR", 0);
  CHECK(r.bits("DLY0CR", "SKPBF") == 0);
  r.write("DLY0CR", 1u << 31);
  CHECK(r.bits("DLY0CR", "SKPBF") == 0);
}

TEST_CASE("CKABF is set at SITFEN without a CCK, again after a clear, and DFLTxISR = ~0 clears it")
{
  rig const r;
  r.write("CKGCR", (1u << 5) | 1u);
  r.write("SITF0CR", 0x1F11);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 1);
  r.write("DFLT0ISR", 1u << 10);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 1);
  r.put("CKGCR", "CCK0EN", 1);
  r.write("DFLT0ISR", 0xFFFF'FFFF);
  CHECK(r.read("DFLT0ISR") == 0);
  r.write("SITF0CR", 0x1F10);
  r.write("SITF1CR", 0x1F01);
  CHECK(r.bits("DFLT1ISR", "CKABF") == 0);
}

TEST_CASE(
    "CKABF is not set with the CCK generated, popcorn's zero latency, rises STH proc cycles "
    "after the CCK stops")
{
  rig const r;
  r.clocks(4, 3);
  r.write("SITF0CR", 0x1F11);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 0);
  r.put("CKGCR", "CCK0EN", 0);
  auto const stopped = r.now();
  auto const proc_ps = 5 * 250'000ull;
  r.run_until(stopped + 31 * proc_ps - us);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 0);
  r.run_until(stopped + 31 * proc_ps);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 1);
  r.put("CKGCR", "CCK0EN", 1);
  r.write("DFLT0ISR", 1u << 10);
  r.put("CKGCR", "CCK0EN", 0);
  r.put("CKGCR", "CCK0EN", 1);
  r.run_until(r.now() + 100 * us);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 0);
}

TEST_CASE("CKABIE with CKABF raises MDF1_FLT0_IRQn, ADF1's raises ADF1_IRQn")
{
  rig const r;
  r.write("CKGCR", (1u << 5) | 1u);
  r.write("SITF0CR", 0x1F11);
  CHECK_FALSE(r.pending(mdf_irq));
  r.write("DFLT0IER", 1u << 10);
  CHECK(r.pending(mdf_irq));
  r.write("ADF1", "CKGCR", (1u << 5) | 1u);
  r.write("ADF1", "SITF0CR", 0x1F11);
  CHECK_FALSE(r.pending(adf_irq));
  r.write("ADF1", "DFLT0IER", 1u << 10);
  CHECK(r.pending(adf_irq));
}

TEST_CASE("ADF1 DFLTEN reads 0 as RM0456 40.8.5 says, a read-modify-write stops the filter")
{
  rig const r;
  r.write("ADF1", "DFLT0CICR", 5u << 4);
  r.write("ADF1", "DFLT0CR", 1);
  CHECK(r.field("ADF1", "DFLT0CR", "DFLTACTIVE") == 1);
  CHECK((r.read("ADF1", "DFLT0CR") & 1u) == 0);
  r.write("ADF1", "DFLT0CR", r.read("ADF1", "DFLT0CR") | (1u << 1));
  CHECK(r.field("ADF1", "DFLT0CR", "DFLTACTIVE") == 0);
}

TEST_CASE(
    "the CCK limits of Table 374: 5 MHz in LF_MASTER, 25 MHz in normal SPI, hclk at least proc")
{
  {
    rig const r;
    r.pll3_kernel(1, 25, 10);
    r.clocks(0, 7);
    r.write("SITF0CR", 0x1F01);
    CHECK(r.warned("above the") == 0);
    CHECK(r.warned("hclk") == 1);
  }
  {
    rig const r;
    r.pll3_kernel(1, 25, 10);
    r.clocks(0, 6);
    r.write("SITF0CR", 0x1F01);
    CHECK(r.warned("above the 5000000 Hz") == 1);
  }
  {
    rig const r;
    r.pll3_kernel(1, 25, 4);
    r.clocks(0, 3);
    r.write("SITF0CR", 0x1F11);
    CHECK(r.warned("above the") == 0);
  }
  rig const r;
  r.pll3_kernel(1, 25, 4);
  r.clocks(0, 2);
  r.write("SITF0CR", 0x1F11);
  CHECK(r.warned("above the 25000000 Hz") == 1);
}

TEST_CASE("a byte write of 0x04 to DFLTxISR byte 1 clears CKABF only")
{
  rig const r;
  r.write("CKGCR", (1u << 5) | 1u);
  r.write("SITF0CR", 0x1F11);
  r.put("CKGCR", "CCK0EN", 1);
  REQUIRE(r.bits("DFLT0ISR", "CKABF") == 1);
  REQUIRE(r.target->mem().write(r.at("MDF1", "DFLT0ISR"), 1, 0xFF).has_value());
  CHECK(r.bits("DFLT0ISR", "CKABF") == 1);
  REQUIRE(r.target->mem().write(r.at("MDF1", "DFLT0ISR") + 1, 1, 0x04).has_value());
  CHECK(r.bits("DFLT0ISR", "CKABF") == 0);
}

TEST_CASE("CKGDEN 1 to 0 under a running normal SPI SITF sets CKABF STH proc cycles later")
{
  rig const r;
  r.clocks(4, 3);
  r.write("SITF0CR", 0x1F11);
  r.put("CKGCR", "CKGDEN", 0);
  auto const stopped = r.now();
  r.run_until(stopped + 31 * 1'250'000ull - us);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 0);
  r.run_until(stopped + 31 * 1'250'000ull);
  CHECK(r.bits("DFLT0ISR", "CKABF") == 1);
}

TEST_CASE("BSSEL past the last SITF selects SITF5: its clocks are checked at DFLT0 DFLTEN")
{
  rig const r;
  r.clocks(0, 3);
  r.write("SITF5CR", 0x1F11);
  r.write("BSMX0CR", 31);
  r.write("DFLT0CICR", 3u << 8);
  r.write("DFLT0CR", 1);
  CHECK(r.warned("RSFLT") == 1);
}

TEST_CASE("CKABIE on filters 4 and 5 pends IRQ 121 and 122, a cleared flag lowers the line")
{
  rig const r;
  r.write("CKGCR", (1u << 5) | 1u);
  for(auto const f : {4, 5})
  {
    r.write(std::format("SITF{}CR", f), 0x1F11);
    r.write(std::format("DFLT{}IER", f), 1u << 10);
  }
  CHECK(r.pending(121));
  CHECK(r.pending(122));
  CHECK_FALSE(r.pending(mdf_irq));
  r.put("CKGCR", "CCK0EN", 1);
  r.write("DFLT5ISR", 1u << 10);
  REQUIRE(r.target->mem().write(0xe000'e280u + 4 * (122 / 32), 4, 1u << (122 % 32)).has_value());
  CHECK_FALSE(r.pending(122));
  REQUIRE(r.target->mem().write(0xe000'e280u + 4 * (121 / 32), 4, 1u << (121 % 32)).has_value());
  CHECK(r.pending(121));
}

namespace
{
constexpr std::uint64_t peer_divider = 10;
constexpr std::uint64_t peer_kernel  = 20'480'000;

class scripted final : public stm32::pdm_source
{
public:
  std::vector<std::uint8_t> bits;
  std::uint8_t              fill{};
  std::size_t               next{};
  std::uint64_t             pulled{};
  bool                      switched{};
  std::uint64_t             since{};

  void clock(std::span<std::uint8_t> out, stm32::bit_rate rate) override
  {
    CHECK(rate.divider == peer_divider);
    for(auto& bit : out)
    {
      if(switched)
      {
        bit = since++ % 3 == 0 ? 1 : 0;
      }
      else
      {
        bit = next < bits.size() ? bits[next] : fill;
        ++next;
      }
    }
    pulled += out.size();
  }
};

struct tapped
{
  std::int32_t  pcm;
  std::uint64_t at;
  bool          dropped;
};

class recorder final : public stm32::pdm_tap
{
public:
  std::vector<tapped> samples;

  void wrote(std::int32_t pcm, vtime at, bool dropped, bool captured) override
  {
    CHECK(captured);
    samples.push_back({.pcm = pcm, .at = at.ps, .dropped = dropped});
  }
};

auto vectors() -> std::filesystem::path
{
  char const* const value = std::getenv("POPCORN_MDF_VECTORS");
  REQUIRE(value != nullptr);
  return value;
}

struct vector_run
{
  std::uint32_t                order{};
  std::uint32_t                decimation{};
  std::uint32_t                scale{};
  bool                         rsflt{};
  bool                         rsflt_decimate{};
  std::optional<std::uint32_t> hpf;
  std::optional<std::uint32_t> int_value;
  std::uint32_t                int_div{};
  std::uint32_t                discard{};
  std::uint32_t                skip{};
  std::vector<std::uint8_t>    bits;
  std::vector<std::int32_t>    pcm;
  std::vector<bool>            saturated;
};

auto vector_runs(std::string_view name) -> std::vector<vector_run>
{
  namespace json = reflex::serde::json;
  std::ifstream input(vectors() / std::format("chain_{}.json", name), std::ios::binary);
  REQUIRE(input);
  std::ostringstream text;
  text << input.rdbuf();
  auto document = json::deserializer{std::move(text).str()}.load<json::value>();
  auto number   = [](json::value const& value) {
    return static_cast<std::uint32_t>(value.as<json::number>());
  };
  auto optional = [&](json::value const& value) -> std::optional<std::uint32_t> {
    if(value.is_null())
    {
      return std::nullopt;
    }
    return number(value);
  };
  std::vector<vector_run> out;
  for(auto& run : document["runs"].as_array())
  {
    auto&      config = run["config"];
    vector_run got{
        .order =
            static_cast<std::uint32_t>(std::stoul(std::string{config["order"].as<json::string>()})),
        .decimation     = number(config["decimation"]),
        .scale          = number(config["scale"]),
        .rsflt          = config["rsflt"].as<json::boolean>(),
        .rsflt_decimate = config["rsflt_decimate"].as<json::boolean>(),
        .hpf            = optional(config["hpf"]),
        .int_value      = optional(config["int_value"]),
        .int_div        = number(config["int_div"]),
        .discard        = number(config["discard"]),
        .skip           = number(config["skip"]),
        .bits           = {},
        .pcm            = {},
        .saturated      = {}};
    auto const& hex   = run["bits"]["hex"].as<json::string>();
    auto const  count = static_cast<std::size_t>(run["bits"]["count"].as<json::number>());
    for(std::size_t i = 0; i < count; ++i)
    {
      got.bits.push_back((std::stoul(hex.substr(i / 8 * 2, 2), nullptr, 16) >> (7 - i % 8)) & 1u);
    }
    for(auto const& pair : run["pcm"].as_array())
    {
      got.pcm.insert(
          got.pcm.end(), static_cast<std::size_t>(pair[1].as<json::number>()),
          static_cast<std::int32_t>(pair[0].as<json::number>()));
    }
    for(auto const& pair : run["saturated"].as_array())
    {
      got.saturated.insert(
          got.saturated.end(), static_cast<std::size_t>(pair[1].as<json::number>()),
          pair[0].as<json::number>() != 0);
    }
    out.push_back(std::move(got));
  }
  return out;
}

auto pdm_16k() -> std::pair<std::vector<std::uint8_t>, std::vector<std::int32_t>>
{
  namespace json = reflex::serde::json;
  std::ifstream input(vectors() / "chain_pdm_16k.json", std::ios::binary);
  REQUIRE(input);
  std::ostringstream text;
  text << input.rdbuf();
  auto        document = json::deserializer{std::move(text).str()}.load<json::value>();
  auto&       run      = document["runs"][0];
  auto const& hex      = run["bits"]["hex"].as<json::string>();
  auto const  count    = static_cast<std::size_t>(run["bits"]["count"].as<json::number>());
  std::vector<std::uint8_t> bits(count);
  for(std::size_t i = 0; i < count; ++i)
  {
    bits[i] = (std::stoul(hex.substr(i / 8 * 2, 2), nullptr, 16) >> (7 - i % 8)) & 1u;
  }
  std::vector<std::int32_t> pcm;
  for(auto const& pair : run["pcm"].as_array())
  {
    pcm.insert(
        pcm.end(), static_cast<std::size_t>(pair[1].as<json::number>()),
        static_cast<std::int32_t>(pair[0].as<json::number>()));
  }
  return {bits, pcm};
}

auto edge_at(std::uint64_t anchor, std::uint64_t h) -> std::uint64_t
{
  auto const num = static_cast<unsigned __int128>(h) * peer_divider * 1'000'000'000'000u;
  return anchor + static_cast<std::uint64_t>(num / (2u * peer_kernel));
}

struct stream_rig : rig
{
  std::array<std::shared_ptr<scripted>, 3> mic{
      std::make_shared<scripted>(), std::make_shared<scripted>(), std::make_shared<scripted>()};
  std::array<std::shared_ptr<recorder>, 6> taps{};
  std::uint64_t                            anchor{};

  explicit stream_rig(bool probe = false) : rig{false, probe}
  {
    pll3_kernel(5, 64, 10);
    port().attach(0, stm32::pdm_edge::rising, mic[0]);
    port().attach(0, stm32::pdm_edge::falling, mic[1]);
    port().attach(1, stm32::pdm_edge::rising, mic[2]);
    for(std::size_t f = 0; f < taps.size(); ++f)
    {
      taps[f] = std::make_shared<recorder>();
      port().attach_tap(f, taps[f]);
    }
    write("CKGCR", (4u << 24) | (1u << 16) | (1u << 5) | (1u << 1));
    put("CKGCR", "CKGDEN", 1);
    anchor = now();
    write("SITF0CR", 0x1F11);
  }

  [[nodiscard]] auto port() const -> stm32::pdm_port&
  {
    auto* const found = dynamic_cast<stm32::pdm_port*>(target->model("MDF1"));
    REQUIRE(found != nullptr);
    return *found;
  }

  void sinc1(
      std::size_t   f,
      std::uint32_t stream,
      std::uint32_t scale = 0,
      std::uint32_t mode  = 0,
      std::uint32_t extra = 0) const
  {
    write(std::format("BSMX{}CR", f), stream);
    write(std::format("DFLT{}CICR", f), (scale << 20) | (1u << 8) | (1u << 4));
    write(std::format("DFLT{}RSFR", f), 1u | (1u << 7));
    write(std::format("DFLT{}CR", f), extra | (mode << 4) | 1u);
  }

  [[nodiscard]] auto pop(std::uint32_t lane = 0, std::uint8_t size = 4) const -> std::uint32_t
  {
    auto&       regs  = target->peripheral("MDF1");
    auto const* found = popcorn::find_register(regs.description(), "DFLT0DR");
    REQUIRE(found != nullptr);
    auto const value = target->model("MDF1")->on_read(
        regs, found->offset + lane, size,
        {.pc = 0, .now = icount{0}, .by = popcorn::initiator::cpu});
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto requested() const -> std::vector<bool>
  {
    auto* const probe = model_of<request_probe>("GPDMA1");
    REQUIRE(probe != nullptr);
    return probe->levels[flt0_request];
  }

  [[nodiscard]] auto requesting() const -> bool
  {
    auto const seen = requested();
    return not seen.empty() and seen.back();
  }
};
}

TEST_CASE("bit edges follow the closed form of the peer clock, 244140.625 ps half periods")
{
  stream_rig const r;
  r.sinc1(0, 0);
  r.run_until(r.anchor + 1100 * us);
  auto const& got = r.taps[0]->samples;
  REQUIRE(got.size() > 1024);
  for(auto const k : {0u, 1u, 1023u, 1024u})
  {
    CAPTURE(k);
    CHECK(got[k].at == edge_at(r.anchor, 4 * std::uint64_t{k} + 4));
  }
  CHECK(edge_at(r.anchor, 1) - r.anchor == 244'140);
  CHECK(edge_at(r.anchor, 8) - r.anchor == 1'953'125);
}

TEST_CASE("the peer's chain on SITF0 reproduces chain_pdm_16k.json at 128-bit edges")
{
  stream_rig const r;
  auto [bits, pcm] = pdm_16k();
  r.mic[0]->bits   = bits;
  r.write("BSMX0CR", 0);
  r.write("DFLT0CICR", (0x2Cu << 20) | (31u << 8) | (5u << 4));
  r.write("DFLT0RSFR", 2u << 8);
  r.write("DFLT0CR", 1);
  r.run_until(edge_at(r.anchor, 2 + 2 * (bits.size() - 1)) + us);
  auto const& got = r.taps[0]->samples;
  REQUIRE(got.size() >= pcm.size());
  for(std::size_t k = 0; k < pcm.size(); ++k)
  {
    CAPTURE(k);
    CHECK(got[k].pcm == pcm[k]);
    CHECK(got[k].at == edge_at(r.anchor, 2 + 2 * (128 * (k + 1) - 1)));
  }
}

TEST_CASE("bs0_r and bs0_f reach their filters, two filters on one stream share the pull")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.mic[1]->fill = 0;
  r.sinc1(0, 0);
  r.sinc1(1, 1);
  r.sinc1(2, 0);
  r.run_until(r.anchor + 100 * us);
  auto const& rising  = r.taps[0]->samples;
  auto const& falling = r.taps[1]->samples;
  REQUIRE(rising.size() > 10);
  CHECK(rising[0].pcm == 2);
  CHECK(falling[0].pcm == -2);
  REQUIRE(r.taps[2]->samples.size() == rising.size());
  for(std::size_t k = 0; k < rising.size(); ++k)
  {
    CHECK(r.taps[2]->samples[k].pcm == rising[k].pcm);
    CHECK(r.taps[2]->samples[k].at == rising[k].at);
  }
  CHECK(r.mic[0]->pulled <= r.mic[1]->pulled + 1);
  CHECK(r.mic[1]->pulled <= r.mic[0]->pulled + 1);
}

TEST_CASE("a fifth sample with no read overruns: DOVRF, the sample dropped, the tap marks it")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.sinc1(0, 0);
  r.run_until(edge_at(r.anchor, 4 * 4 + 4) - us / 2);
  REQUIRE(r.taps[0]->samples.size() == 4);
  CHECK(r.bits("DFLT0ISR", "DOVRF") == 0);
  r.run_until(edge_at(r.anchor, 4 * 4 + 4) + us / 4);
  auto const& got = r.taps[0]->samples;
  REQUIRE(got.size() >= 5);
  CHECK_FALSE(got[3].dropped);
  CHECK(got[4].dropped);
  CHECK(r.bits("DFLT0ISR", "DOVRF") == 1);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK(r.bits("DFLT0ISR", "FTHF") == 1);
}

TEST_CASE("DFLTxDR pops one sample per read of any size, PCM in bits 31:8, empty reads 0")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.sinc1(0, 0, 0x18);
  r.run_until(edge_at(r.anchor, 4 * 2 + 4) + 1);
  REQUIRE(r.taps[0]->samples.size() >= 2);
  CHECK(r.pop() == 0x0020'0000u);
  CHECK(r.pop(2, 2) == 0x20u);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK(r.pop() == 0x0020'0000u);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 0);
  CHECK(r.pop() == 0);
}

TEST_CASE("single-shot writes one sample, DFLTRUN 0, a DFLTEN write restarts from reset taps")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.sinc1(0, 0, 0, 1);
  CHECK(r.bits("DFLT0CR", "DFLTRUN") == 1);
  r.run_until(r.anchor + 50 * us);
  CHECK(r.taps[0]->samples.size() == 1);
  CHECK(r.bits("DFLT0CR", "DFLTRUN") == 0);
  CHECK(r.bits("DFLT0CR", "DFLTACTIVE") == 1);
  auto const again = r.now();
  r.write("DFLT0CR", (1u << 4) | 1u);
  r.run_until(again + 50 * us);
  REQUIRE(r.taps[0]->samples.size() == 2);
  CHECK(r.taps[0]->samples[1].pcm == 2);
  CHECK(r.taps[0]->samples[1].at > again);
}

TEST_CASE(
    "settle clocks the edges before a source change: later edges, 100 ms on, see the new content")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.run_until(r.anchor + 1000 * us);
  auto const t1 = r.now();
  r.port().settle();
  r.mic[0]->switched = true;
  r.run_until(t1 + 100'000 * us);
  auto const t2 = r.now();
  r.sinc1(0, 0);
  r.run_until(t2 + 100 * us);
  auto const before = [&](std::uint64_t t) {
    auto const num = static_cast<unsigned __int128>(t - r.anchor + 1) * 2u * peer_kernel;
    auto const den = static_cast<unsigned __int128>(peer_divider) * 1'000'000'000'000u;
    auto const end = static_cast<std::uint64_t>((num + den - 1) / den);
    return (end - 1) / 2;
  };
  auto const  skipped = before(t2) - before(t1);
  auto const  bit     = [&](std::uint64_t k) { return (skipped + k) % 3 == 0 ? 1 : -1; };
  auto const& got     = r.taps[0]->samples;
  REQUIRE(got.size() > 8);
  for(std::uint64_t k = 0; k < 8; ++k)
  {
    CAPTURE(k);
    CHECK(got[k].pcm == bit(2 * k) + bit(2 * k + 1));
  }
}

TEST_CASE("CCK0EN 0 then 1 with CKGDEN 1 restarts the bit clock at the write, new anchor")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.run_until(r.anchor + 10 * us);
  r.put("CKGCR", "CCK0EN", 0);
  r.run_until(r.now() + 3 * us);
  auto const restart = r.now();
  r.put("CKGCR", "CCK0EN", 1);
  r.sinc1(0, 0);
  r.run_until(restart + 20 * us);
  auto const& got = r.taps[0]->samples;
  REQUIRE(got.size() >= 4);
  for(std::uint64_t k = 0; k < 4; ++k)
  {
    CAPTURE(k);
    CHECK(got[k].at == edge_at(restart, 4 * k + 4));
  }
}

TEST_CASE("a kernel clock change re-anchors at the next edge: no bit skipped or doubled")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.sinc1(0, 0);
  r.run_until(r.anchor + 100 * us + 77'000);
  auto const changed = r.now();
  r.put("RCC", "RCC_CCIPR2", "MDF1SEL", 0);
  r.run_until(changed + 100 * us);
  std::uint64_t next = 0;
  while(edge_at(r.anchor, next) <= changed)
  {
    ++next;
  }
  auto const rebased  = edge_at(r.anchor, next);
  auto const expected = [&](std::uint64_t h) {
    if(h < next)
    {
      return edge_at(r.anchor, h);
    }
    auto const num = static_cast<unsigned __int128>(h - next) * peer_divider * 1'000'000'000'000u;
    return rebased + static_cast<std::uint64_t>(num / (2u * 4'000'000u));
  };
  auto const& got = r.taps[0]->samples;
  REQUIRE(got.size() > next / 4 + 10);
  for(std::size_t k = 0; k < got.size(); ++k)
  {
    CAPTURE(k);
    CHECK(got[k].at == expected(4 * k + 4));
  }
}

namespace
{
constexpr std::uint32_t dmaen  = 1u << 1;
constexpr std::uint32_t fth    = 1u << 2;
constexpr std::uint32_t rfovrf = 1u << 11;

void rsflt_at(stream_rig const& r, std::uint32_t mcicd)
{
  r.write("DFLT0IER", rfovrf);
  r.write("DFLT0CICR", (mcicd << 8) | (5u << 4));
  r.write("DFLT0RSFR", 1u << 7);
  r.write("DFLT0CR", 1);
}
}

TEST_CASE("with DMAEN and FTH 0 the DMA request follows RXNEF, without DMAEN it stays low")
{
  {
    stream_rig const r{true};
    r.mic[0]->fill = 1;
    r.sinc1(0, 0, 0, 0, dmaen);
    r.run_until(edge_at(r.anchor, 4) - us / 2);
    CHECK_FALSE(r.requesting());
    r.run_until(edge_at(r.anchor, 4) + us / 4);
    CHECK(r.requesting());
    static_cast<void>(r.pop());
    CHECK_FALSE(r.requesting());
    r.run_until(edge_at(r.anchor, 12) + us / 4);
    CHECK(r.requesting());
    r.write("DFLT0CR", 0);
    CHECK_FALSE(r.requesting());
    CHECK(r.requested() == std::vector<bool>{false, true, false, true, false});
  }
  stream_rig const r{true};
  r.mic[0]->fill = 1;
  r.sinc1(0, 0);
  r.run_until(edge_at(r.anchor, 12) + us / 4);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK_FALSE(std::ranges::contains(r.requested(), true));
}

TEST_CASE("with FTH 1 the request rises at the second sample and holds until the RXFIFO empties")
{
  stream_rig const r{true};
  r.mic[0]->fill = 1;
  r.sinc1(0, 0, 0, 0, dmaen | fth);
  r.run_until(edge_at(r.anchor, 4) + us / 4);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK(r.bits("DFLT0ISR", "FTHF") == 0);
  CHECK_FALSE(r.requesting());
  r.run_until(edge_at(r.anchor, 8) + us / 4);
  CHECK(r.bits("DFLT0ISR", "FTHF") == 1);
  CHECK(r.requesting());
  static_cast<void>(r.pop());
  CHECK(r.bits("DFLT0ISR", "FTHF") == 0);
  CHECK(r.requesting());
  static_cast<void>(r.pop());
  CHECK_FALSE(r.requesting());
  r.run_until(edge_at(r.anchor, 12) + us / 4);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK_FALSE(r.requesting());
  r.run_until(edge_at(r.anchor, 16) + us / 4);
  CHECK(r.requesting());
}

TEST_CASE(
    "RFOVRF and its IRQ rise at each RSFLT input after the first while (MCICD + 1) x "
    "(CCKDIV + 1) < 24")
{
  {
    stream_rig const r;
    r.mic[0]->fill = 1;
    rsflt_at(r, 10);
    CHECK(r.warned("RSFLT needs") == 1);
    r.run_until(edge_at(r.anchor, 22) + us / 4);
    CHECK_FALSE(r.pending(mdf_irq));
    CHECK((r.read("DFLT0ISR") & rfovrf) == 0);
    r.run_until(edge_at(r.anchor, 44) - us / 2);
    CHECK_FALSE(r.pending(mdf_irq));
    CHECK((r.read("DFLT0ISR") & rfovrf) == 0);
    r.run_until(edge_at(r.anchor, 44) + us / 4);
    CHECK(r.pending(mdf_irq));
    CHECK((r.read("DFLT0ISR") & rfovrf) != 0);
    CHECK(r.taps[0]->samples.empty());
    r.write("DFLT0ISR", rfovrf);
    CHECK((r.read("DFLT0ISR") & rfovrf) == 0);
    r.run_until(edge_at(r.anchor, 66) - us / 2);
    CHECK((r.read("DFLT0ISR") & rfovrf) == 0);
    r.run_until(edge_at(r.anchor, 66) + us / 4);
    CHECK((r.read("DFLT0ISR") & rfovrf) != 0);
    r.run_until(edge_at(r.anchor, 88) + us / 4);
    CHECK(r.taps[0]->samples.size() == 1);
  }
  stream_rig const r;
  r.mic[0]->fill = 1;
  rsflt_at(r, 11);
  CHECK(r.warned("RSFLT needs") == 0);
  r.run_until(r.anchor + 200 * us);
  CHECK(r.taps[0]->samples.size() > 5);
  CHECK((r.read("DFLT0ISR") & rfovrf) == 0);
  CHECK_FALSE(r.pending(mdf_irq));
}

TEST_CASE("the peer's chain read between slices of three lengths gives the same samples and times")
{
  auto const [bits, pcm] = pdm_16k();
  std::vector<std::vector<tapped>> runs;
  for(std::uint64_t const step : {1'234'567u, 7'777'777u, 100'000'000u})
  {
    stream_rig const r;
    r.mic[0]->bits = bits;
    r.write("DFLT0CICR", (0x2Cu << 20) | (31u << 8) | (5u << 4));
    r.write("DFLT0RSFR", 2u << 8);
    r.write("DFLT0CR", 1);
    auto const end = r.anchor + 3'990 * us;
    while(r.now() < end)
    {
      r.run_until(std::min(end, r.now() + step));
      static_cast<void>(r.read("DFLT0ISR"));
    }
    auto got = r.taps[0]->samples;
    for(auto& sample : got)
    {
      sample.at -= r.anchor;
    }
    runs.push_back(std::move(got));
  }
  for(auto const& run : runs)
  {
    REQUIRE(run.size() == 63);
    for(std::size_t k = 0; k < run.size(); ++k)
    {
      CAPTURE(k);
      CHECK(run[k].pcm == pcm[k]);
      CHECK(run[k].at == edge_at(0, 2 + 2 * (128 * (k + 1) - 1)));
      CHECK(run[k].dropped == (k >= 4));
    }
  }
}

TEST_CASE("bench: one virtual second of the peer's two filters on silent sensors" * doctest::skip())
{
  std::vector<double> walls;
  for(int run = 0; run < 5; ++run)
  {
    stream_rig const r;
    r.port().attach(0, stm32::pdm_edge::rising, nullptr);
    r.port().attach(0, stm32::pdm_edge::falling, nullptr);
    r.port().attach(1, stm32::pdm_edge::rising, nullptr);
    r.write("SITF1CR", 0x1F11);
    for(std::uint32_t f = 0; f < 2; ++f)
    {
      r.write(std::format("BSMX{}CR", f), f == 0 ? 1u : 2u);
      r.write(std::format("DFLT{}CICR", f), (0x2Cu << 20) | (31u << 8) | (5u << 4));
      r.write(std::format("DFLT{}RSFR", f), 2u << 8);
      r.write(std::format("DFLT{}CR", f), dmaen | 1u);
    }
    auto const start = std::chrono::steady_clock::now();
    r.run_until(r.anchor + 1'000'000 * us);
    walls.push_back(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    CHECK(r.taps[0]->samples.size() == 16'000);
    CHECK(r.taps[1]->samples.size() == 16'000);
  }
  std::ranges::sort(walls);
  MESSAGE(
      std::format(
          "wall s per virtual s: median {:.3f} min {:.3f} max {:.3f}", walls[2], walls[0],
          walls[4]));
}

TEST_CASE("a debugger read of DFLTxDR returns the head and pops nothing, a CPU read pops it")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.sinc1(0, 0, 0x18);
  r.run_until(edge_at(r.anchor, 4) + us / 4);
  CHECK(r.read("DFLT0DR") == 0x0020'0000u);
  CHECK(r.read("DFLT0DR") == 0x0020'0000u);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK(r.pop() == 0x0020'0000u);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 0);
  CHECK(r.read("DFLT0DR") == 0);
}

TEST_CASE("the sample event is due at the closed-form edge for a start on either edge parity")
{
  for(std::uint64_t const parity : {0u, 1u})
  {
    CAPTURE(parity);
    stream_rig const r;
    r.mic[0]->fill     = 1;
    auto const settled = [&r] {
      auto const num = static_cast<unsigned __int128>(r.now() - r.anchor + 1) * 2u * peer_kernel;
      auto const den = static_cast<unsigned __int128>(peer_divider) * 1'000'000'000'000u;
      return static_cast<std::uint64_t>((num + den - 1) / den);
    };
    r.run_until(r.anchor + 10 * us);
    while(settled() % 2 != parity)
    {
      r.run_until(r.now() + 100'000);
    }
    auto const next = settled();
    r.sinc1(0, 0);
    auto const due  = edge_at(r.anchor, next + (next & 1u) + 2);
    auto const snap = r.target->snapshot();
    CHECK(std::ranges::any_of(snap.data().events.pending, [due](core::event const& e) {
      return e.due_ps == due;
    }));
    r.run_until(due + us / 4);
    REQUIRE(r.taps[0]->samples.size() == 1);
    CHECK(r.taps[0]->samples[0].at == due);
  }
}

namespace
{
constexpr std::uint32_t satf            = 1u << 9;
constexpr std::uint32_t rxnef           = 1u << 3;
constexpr std::uint32_t sync_continuous = 2u << 4;
constexpr std::uint32_t sync_single     = 3u << 4;
constexpr std::uint32_t trgo            = 1u;
constexpr std::uint32_t ilvnb1          = 1u << 4;

auto int_code(std::uint32_t ratio) -> std::uint32_t
{
  switch(ratio)
  {
    case 32:
      return 1;
    case 4:
      return 2;
    case 1:
      return 3;
    default:
      return 0;
  }
}

void program(stream_rig const& r, vector_run const& v, std::size_t f = 0, std::uint32_t cr = 0)
{
  r.write(
      std::format("DFLT{}CICR", f), (v.scale << 20) | ((v.decimation - 1) << 8) | (v.order << 4));
  r.write(
      std::format("DFLT{}RSFR", f),
      (v.rsflt ? 0u : 1u) | (v.rsflt_decimate ? 0u : 1u << 4) | (v.hpf ? *v.hpf << 8 : 1u << 7));
  r.write(
      std::format("DFLT{}INTR", f),
      (v.int_value ? (*v.int_value - 1) << 4 : 0u) | int_code(v.int_div));
  if(v.skip != 0)
  {
    r.write(std::format("DLY{}CR", f), v.skip);
  }
  r.write(std::format("DFLT{}CR", f), (v.discard << 20) | cr | 1u);
}

auto step_of(vector_run const& v) -> std::uint64_t
{
  auto const bits =
      v.decimation * (v.rsflt and v.rsflt_decimate ? 4u : 1u) * v.int_value.value_or(1u) / 2u;
  return edge_at(0, 2 * std::uint64_t{bits});
}

auto end_of(stream_rig const& r, vector_run const& v) -> std::uint64_t
{
  return r.anchor + edge_at(0, 2 * v.bits.size() + 2) + us;
}

void step_until(
    stream_rig const&  r,
    std::uint64_t      end,
    std::uint64_t      step,
    std::vector<bool>& saturated)
{
  while(r.now() < end)
  {
    r.run_until(std::min(end, r.now() + step));
    auto const isr  = r.read("DFLT0ISR");
    auto const seen = r.taps[0]->samples.size();
    if(seen == saturated.size())
    {
      continue;
    }
    REQUIRE(seen == saturated.size() + 1);
    saturated.push_back((isr & satf) != 0);
    if((isr & satf) != 0)
    {
      r.write("DFLT0ISR", satf);
    }
  }
}

auto edges_now(stream_rig const& r) -> std::uint64_t
{
  auto const num = static_cast<unsigned __int128>(r.now() - r.anchor + 1) * 2u * peer_kernel;
  auto const den = static_cast<unsigned __int128>(peer_divider) * 1'000'000'000'000u;
  return static_cast<std::uint64_t>((num + den - 1) / den);
}

auto pcm_of(std::uint32_t word) -> std::int32_t
{
  return static_cast<std::int32_t>(word) >> 8;
}
}

TEST_CASE("discard_skip, int and int_bypass through DFLT0: every PCM and its SATF")
{
  for(auto const* const name : {"discard_skip", "int", "int_bypass"})
  {
    std::size_t index = 0;
    for(auto const& v : vector_runs(name))
    {
      CAPTURE(name);
      CAPTURE(index);
      ++index;
      stream_rig const r;
      r.mic[0]->bits = v.bits;
      program(r, v);
      std::vector<bool> saturated;
      step_until(r, end_of(r, v), step_of(v), saturated);
      auto const& got = r.taps[0]->samples;
      REQUIRE(got.size() >= v.pcm.size());
      for(std::size_t k = 0; k < v.pcm.size(); ++k)
      {
        CAPTURE(k);
        CHECK(got[k].pcm == v.pcm[k]);
        CHECK(saturated[k] == v.saturated[k]);
      }
    }
  }
}

TEST_CASE("a snapshot taken after the discards, before the first kept sample, keeps its SATF carry")
{
  auto const       v = vector_runs("discard_skip").at(3);
  stream_rig const r;
  r.mic[0]->bits = v.bits;
  program(r, v);
  auto const end  = end_of(r, v);
  auto const step = step_of(v);
  r.run_until(r.anchor + edge_at(0, 2 * (32 * 32 + 16)));
  r.port().settle();
  REQUIRE(r.taps[0]->samples.empty());
  auto const        saved = r.target->snapshot();
  auto const        next  = r.mic[0]->next;
  std::vector<bool> first;
  step_until(r, end, step, first);
  auto const first_pcm = r.taps[0]->samples;
  r.target->restore(saved);
  r.mic[0]->next = next;
  r.taps[0]->samples.clear();
  std::vector<bool> second;
  step_until(r, end, step, second);
  REQUIRE(first.size() >= v.pcm.size());
  REQUIRE(second.size() == first.size());
  for(std::size_t k = 0; k < v.pcm.size(); ++k)
  {
    CAPTURE(k);
    CHECK(r.taps[0]->samples[k].pcm == first_pcm[k].pcm);
    CHECK(second[k] == v.saturated[k]);
    CHECK(first[k] == v.saturated[k]);
  }
}

TEST_CASE("SKPDLY delays the samples by that many bits, SKPBF reads 1 until the last skipped edge")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.write("DLY0CR", 3);
  CHECK(r.read("DLY0CR") == ((1u << 31) | 3u));
  r.sinc1(0, 0);
  r.run_until(edge_at(r.anchor, 6) - us / 2);
  CHECK((r.read("DLY0CR") >> 31) == 1);
  r.run_until(edge_at(r.anchor, 6) + us / 4);
  CHECK((r.read("DLY0CR") >> 31) == 0);
  r.run_until(r.anchor + 10 * us);
  auto const& got = r.taps[0]->samples;
  REQUIRE(got.size() >= 4);
  for(std::uint64_t k = 0; k < 4; ++k)
  {
    CAPTURE(k);
    CHECK(got[k].at == edge_at(r.anchor, 10 + 4 * k));
  }
  auto const written = r.now();
  auto const before  = got.size();
  r.write("DLY0CR", 2);
  r.write("DLY0CR", 5);
  CHECK(r.warned("DLY0CR") == 1);
  r.run_until(written + 10 * us);
  REQUIRE(got.size() > before + 4);
  for(auto k = before; k < got.size(); ++k)
  {
    CAPTURE(k);
    CHECK(got[k].at == edge_at(r.anchor, 10 + 4 * k + 4));
  }
}

TEST_CASE("TRGO starts the armed sync filters at the next edge of their stream, half a bit apart")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.write("GCR", trgo);
  r.sinc1(0, 0, 0, 2);
  r.sinc1(1, 1, 0, 2);
  r.run_until(r.anchor + 20 * us);
  CHECK(r.taps[0]->samples.empty());
  CHECK(r.taps[1]->samples.empty());
  CHECK(r.bits("DFLT0CR", "DFLTRUN") == 0);
  auto const next = edges_now(r);
  r.write("GCR", trgo);
  CHECK(r.bits("GCR", "TRGO") == 0);
  CHECK(r.bits("DFLT0CR", "DFLTRUN") == 1);
  CHECK(r.bits("DFLT1CR", "DFLTRUN") == 1);
  auto const rising  = next + (next & 1u);
  auto const falling = next + ((next ^ 1u) & 1u);
  r.run_until(r.now() + 20 * us);
  r.write("GCR", trgo);
  r.run_until(r.now() + 20 * us);
  auto const& on_rising  = r.taps[0]->samples;
  auto const& on_falling = r.taps[1]->samples;
  REQUIRE(on_rising.size() > 30);
  REQUIRE(on_falling.size() > 30);
  CHECK(on_rising[0].pcm == 2);
  CHECK(on_falling[0].pcm == -2);
  for(std::uint64_t k = 0; k < 30; ++k)
  {
    CAPTURE(k);
    CHECK(on_rising[k].at == edge_at(r.anchor, rising + 2 + 4 * k));
    CHECK(on_falling[k].at == edge_at(r.anchor, falling + 2 + 4 * k));
  }
}

TEST_CASE("sync single-shot: each TRGO gives one sample after NBDIS discarded ones")
{
  stream_rig const r;
  r.mic[0]->fill = 1;
  r.sinc1(0, 0, 0, 3, 2u << 20);
  r.run_until(r.anchor + 5 * us);
  CHECK(r.taps[0]->samples.empty());
  for(std::size_t shot = 0; shot < 2; ++shot)
  {
    CAPTURE(shot);
    auto const next = edges_now(r);
    r.write("GCR", trgo);
    r.run_until(r.now() + 20 * us);
    REQUIRE(r.taps[0]->samples.size() == shot + 1);
    CHECK(r.taps[0]->samples[shot].at == edge_at(r.anchor, next + (next & 1u) + 2 + 4 * 2));
    CHECK(r.bits("DFLT0CR", "DFLTRUN") == 0);
  }
}

TEST_CASE("interleaved DMA: DFLT0's request waits for both RXFIFOs and holds for a round of reads")
{
  stream_rig const r{true};
  r.mic[0]->fill = 1;
  r.write("GCR", ilvnb1);
  r.sinc1(0, 0, 0, 2, dmaen);
  r.sinc1(1, 1, 0, 2, dmaen);
  r.run_until(r.anchor + 5 * us);
  CHECK_FALSE(r.requesting());
  auto const next = edges_now(r);
  r.write("GCR", ilvnb1 | trgo);
  r.run_until(edge_at(r.anchor, next + 3) + us / 4);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK(r.bits("DFLT1ISR", "RXNEF") == 1);
  CHECK(r.bits("DFLT0ISR", "FTHF") == 1);
  CHECK(r.bits("DFLT1ISR", "FTHF") == 0);
  CHECK(r.requesting());
  CHECK(pcm_of(r.pop()) == 2);
  CHECK(r.bits("DFLT0ISR", "FTHF") == 0);
  CHECK(r.requesting());
  CHECK(pcm_of(r.pop()) == -2);
  CHECK_FALSE(r.requesting());
  CHECK(r.requested() == std::vector<bool>{false, true, false});
  r.run_until(edge_at(r.anchor, next + 3 + 8) + us / 4);
  CHECK(r.bits("DFLT0ISR", "FTHF") == 1);
  static_cast<void>(r.pop());
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK(r.bits("DFLT0ISR", "FTHF") == 0);
  CHECK(r.requesting());
  static_cast<void>(r.pop());
  CHECK(r.bits("DFLT0ISR", "FTHF") == 1);
  CHECK(r.requesting());
  r.write("DFLT1CR", sync_continuous);
  r.run_until(r.now() + 5 * us);
  CHECK(r.bits("DFLT0ISR", "RXNEF") == 1);
  CHECK_FALSE(r.requesting());
}

TEST_CASE("interleaved filters with different SCALE: one warning, strict raises and keeps DFLT1CR")
{
  for(bool const strict : {false, true})
  {
    CAPTURE(strict);
    rig const r{strict};
    r.write("GCR", ilvnb1);
    r.write("DFLT0CICR", (1u << 8) | (1u << 4));
    r.write("DFLT1CICR", (2u << 20) | (1u << 8) | (1u << 4));
    r.write("DFLT0CR", sync_continuous | 1u);
    if(strict)
    {
      CHECK_THROWS_AS(
          static_cast<void>(
              r.target->mem().write(r.at("MDF1", "DFLT1CR"), 4, sync_continuous | 1u)),
          popcorn::model_error);
      CHECK(r.bits("DFLT1CR", "DFLTACTIVE") == 0);
      r.write("DFLT1CICR", (1u << 8) | (1u << 4));
      r.write("DFLT1CR", sync_continuous | 1u);
      CHECK(r.bits("DFLT1CR", "DFLTACTIVE") == 1);
    }
    else
    {
      r.write("DFLT1CR", sync_continuous | 1u);
      r.write("DFLT1CR", sync_continuous);
      r.write("DFLT1CR", sync_continuous | 1u);
      CHECK(r.warned("interleaved filters need") == 1);
      CHECK(r.bits("DFLT1CR", "DFLTACTIVE") == 1);
    }
  }
}

namespace
{
void start_peer(stream_rig const& r, vector_run const& first, vector_run const& second)
{
  r.write("SITF0CR", 0x1F11);
  r.write("SITF1CR", 0x1F11);
  r.write("DFLT0CR", sync_continuous | dmaen | 1u);
  r.write("DFLT1CR", sync_continuous | dmaen | 1u);
  r.port().settle();
  r.mic[1]->bits = first.bits;
  r.mic[1]->next = 0;
  r.mic[2]->bits = second.bits;
  r.mic[2]->next = 0;
  r.write("GCR", ilvnb1 | trgo);
}

auto drain_pairs(stream_rig const& r, std::size_t pairs)
    -> std::pair<std::vector<std::int32_t>, std::vector<std::int32_t>>
{
  std::pair<std::vector<std::int32_t>, std::vector<std::int32_t>> out;
  while(out.first.size() < pairs)
  {
    r.run_until(r.now() + 30 * us);
    while((r.read("DFLT0ISR") & rxnef) != 0 and (r.read("DFLT1ISR") & rxnef) != 0)
    {
      out.first.push_back(pcm_of(r.pop()));
      out.second.push_back(pcm_of(r.pop()));
    }
    REQUIRE(r.bits("DFLT0ISR", "DOVRF") == 0);
  }
  return out;
}
}

TEST_CASE("the peer's two filters interleaved on TRGO, read through DFLT0DR, and restarted")
{
  auto const       quiet = vector_runs("pdm_16k").at(0);
  auto const       loud  = vector_runs("pdm_16k_loud").at(0);
  stream_rig const r;
  r.write("GCR", ilvnb1);
  r.write("SITF1CR", 0x1F11);
  r.write("BSMX0CR", 1);
  r.write("BSMX1CR", 2);
  program(r, quiet, 0, sync_continuous | dmaen);
  program(r, loud, 1, sync_continuous | dmaen);
  r.run_until(r.now() + 100 * us);
  CHECK(r.taps[0]->samples.empty());
  CHECK(r.taps[1]->samples.empty());
  for(int run = 0; run < 2; ++run)
  {
    CAPTURE(run);
    start_peer(r, quiet, loud);
    auto const [first, second] = drain_pairs(r, 64);
    for(std::size_t k = 0; k < 64; ++k)
    {
      CAPTURE(k);
      CHECK(first[k] == quiet.pcm[k]);
      CHECK(second[k] == loud.pcm[k]);
    }
    r.run_until(r.now() + 70 * us);
    REQUIRE((r.read("DFLT0ISR") & rxnef) != 0);
    static_cast<void>(r.pop());
    for(std::size_t f = 0; f < 2; ++f)
    {
      r.write(std::format("SITF{}CR", f), 0x1F10);
      r.write(std::format("DFLT{}CR", f), sync_continuous);
    }
  }
}

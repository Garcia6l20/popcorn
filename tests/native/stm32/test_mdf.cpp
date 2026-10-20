#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
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

constexpr std::uint64_t us         = 1'000'000;
constexpr std::uint32_t mdf_irq    = 102;
constexpr std::uint32_t adf_irq    = 112;
constexpr std::uint32_t sec_offset = 0x1000'0000;

auto options_for(bool strict) -> core::machine_options
{
  stm32::board board{};
  auto         options       = popcorn::testing::stm32_options();
  options.models             = stm32::factories(board, u575());
  options.strict_peripherals = strict;
  return options;
}

struct rig : stm32_fixture
{
  std::vector<core::log_record> logged;
  core::hook_handle             sink;

  explicit rig(bool strict = false)
      : stm32_fixture{u575(), "rcc_ll", options_for(strict)},
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
  CHECK(r.bits("DFLT2CR", "DFLTRUN") == 0);
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
struct unmodelled_row
{
  std::string_view                                        peripheral;
  std::vector<std::pair<std::string_view, std::uint32_t>> setup;
  std::string_view                                        reg;
  std::uint32_t                                           value;
  std::string_view                                        error_reg;
  std::string_view                                        field;
  std::uint32_t                                           field_value;
  std::uint32_t                                           valid;
};

auto unmodelled_rows() -> std::vector<unmodelled_row>
{
  return {
      {"MDF1", {},                           "CKGCR",     (1u << 4) | 1u, "CKGCR",     "CKGMOD",  1,    1           },
      {"MDF1", {{"CKGCR", 0x0100'0003u}},    "SITF0CR",   0x1F11,         "CKGCR",     "CCK0DIR", 0,    0x1F10      },
      {"MDF1", {{"SITF0CR", 0x1F11}},        "CKGCR",     0x0100'0003u,   "CKGCR",     "CCK0DIR", 0,    0x0100'0002u},
      {"MDF1", {},                           "SITF0CR",   0x1F21,         "SITF0CR",   "SITFMOD", 2,    0x1F11      },
      {"MDF1", {},                           "SITF0CR",   0x1F35,         "SITF0CR",   "SITFMOD", 3,    0x1F11      },
      {"MDF1", {},                           "SITF0CR",   0x1F15,         "SITF0CR",   "SCKSRC",  2,    0x1F13      },
      {"MDF1", {{"DFLT0CICR", 2}},           "DFLT0CR",   1,              "DFLT0CICR", "DATSRC",  2,    0           },
      {"MDF1", {},                           "DFLT0CR",   0x41,           "DFLT0CR",   "ACQMOD",  4,    0x61        },
      {"MDF1", {},                           "DFLT0CR",   0x51,           "DFLT0CR",   "ACQMOD",  5,    0x01        },
      {"MDF1", {},                           "DFLT0CR",   0x2021,         "DFLT0CR",   "TRGSRC",  2,    0x0021      },
      {"MDF1", {},                           "DFLT0CR",   0x1031,         "DFLT0CR",   "TRGSRC",  1,    0x0031      },
      {"MDF1", {{"DFLT0CICR", 0x19u << 20}}, "DFLT0CR",   1,              "DFLT0CICR", "SCALE",   0x19, 0           },
      {"ADF1", {},                           "DFLT0CR",   1,              "DFLT0CICR", "CICMOD",  0,    0           },
      {"ADF1", {{"DFLT0CICR", 3u << 4}},     "DFLT0CR",   1,              "DFLT0CICR", "CICMOD",  3,    0           },
      {"MDF1", {{"DFLT0CR", 1}},             "DFLT0CICR", 0x30u << 20,    "DFLT0CICR", "SCALE",   0x30, 0x2Cu << 20 },
      {"MDF1", {},                           "OLD0CR",    1,              "OLD0CR",    "OLDEN",   1,    0x2         },
      {"MDF1", {},                           "SCD0CR",    1,              "SCD0CR",    "SCDEN",   1,    0x10        },
      {"MDF1", {{"GCR", 6u << 4}},           "DFLT0CR",   1,              "GCR",       "ILVNB",   6,    0           },
  };
}
}

TEST_CASE("each unmodelled setting raises naming its field and restores the written register")
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
    r.write(row.peripheral, row.reg, row.valid);
    auto const masked = row.valid & ~std::uint32_t{1};
    CHECK((r.read(row.peripheral, row.reg) & masked) == masked);
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

TEST_CASE("CKABF is not set with the CCK generated, rises STH proc cycles after the CCK stops")
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

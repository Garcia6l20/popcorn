#include <popcorn/core/errors.hpp>

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
auto contains(popcorn::error const& e, std::string_view needle) -> bool
{
  return std::string_view{e.what()}.contains(needle);
}
}

TEST_CASE("config_error formats file, line, key and expected")
{
  popcorn::config_error const e{"mcu.toml", 12, "clocks.sysclk", "integer Hz"};
  CHECK(e.file() == "mcu.toml");
  CHECK(e.line() == 12);
  CHECK(e.key() == "clocks.sysclk");
  CHECK(e.expected() == "integer Hz");
  CHECK(contains(e, "mcu.toml"));
  CHECK(contains(e, "12"));
  CHECK(contains(e, "clocks.sysclk"));
  CHECK(contains(e, "integer Hz"));
}

TEST_CASE("image_error formats segment, address and region")
{
  popcorn::image_error const e{".text", 0x0800'0000, "outside flash"};
  CHECK(e.segment() == ".text");
  CHECK(e.addr() == 0x0800'0000);
  CHECK(e.region() == "outside flash");
  CHECK(contains(e, ".text"));
  CHECK(contains(e, "8000000"));
  CHECK(contains(e, "outside flash"));
}

TEST_CASE("symbol_error formats name and close matches")
{
  popcorn::symbol_error const e{
      "HAL_Delya", {"HAL_Delay", "HAL_DeInit"}
  };
  CHECK(e.name() == "HAL_Delya");
  CHECK(e.close_matches() == std::vector<std::string>{"HAL_Delay", "HAL_DeInit"});
  CHECK(contains(e, "HAL_Delya"));
  CHECK(contains(e, "HAL_Delay"));
  CHECK(contains(e, "HAL_DeInit"));
  CHECK(contains(e, "close matches"));
}

TEST_CASE("symbol_error omits the close-matches clause when there are none")
{
  popcorn::symbol_error const e{"HAL_Mystery", {}};
  CHECK(e.close_matches().empty());
  CHECK(contains(e, "HAL_Mystery"));
  CHECK_FALSE(contains(e, "close matches"));
}

TEST_CASE("emulation_error carries a plain message")
{
  popcorn::emulation_error const e{"engine is closed"};
  CHECK(contains(e, "engine is closed"));
}

TEST_CASE("fault_error formats the fault and the symbol")
{
  popcorn::location const where{
      .pc = 0x0800'2000, .symbol = "hard_fault_handler", .offset = 0, .file = {}, .line = 0};
  popcorn::fault_info const fault{
      .kind          = popcorn::fault_kind::bus,
      .fault_address = 0x2000'1000,
      .status        = 0xdead,
      .detail        = 0xbeef};
  popcorn::fault_error const e{where, fault};

  CHECK(e.where().pc == where.pc);
  CHECK(e.where().symbol == "hard_fault_handler");
  CHECK(e.fault().kind == popcorn::fault_kind::bus);
  CHECK(contains(e, "bus"));
  CHECK(contains(e, "8002000"));
  CHECK(contains(e, "hard_fault_handler"));
  CHECK(contains(e, "dead"));
  CHECK(contains(e, "beef"));
  CHECK(contains(e, "20001000"));
}

TEST_CASE("fault_error formats offset and file:line when present")
{
  popcorn::location const where{
      .pc     = 0x0800'2010,
      .symbol = "hard_fault_handler",
      .offset = 0x10,
      .file   = "fault.c",
      .line   = 42};
  popcorn::fault_info const fault{
      .kind = popcorn::fault_kind::usage, .fault_address = 0, .status = 1, .detail = 2};
  popcorn::fault_error const e{where, fault};

  CHECK(e.where().offset == 0x10);
  CHECK(e.where().file == "fault.c");
  CHECK(e.where().line == 42);
  CHECK(contains(e, "+0x10"));
  CHECK(contains(e, "fault.c:42"));
}

TEST_CASE("fault_error names an out-of-range fault kind as unknown(N) instead of throwing")
{
  auto const                bad_kind = static_cast<popcorn::fault_kind>(0xff);
  popcorn::location const   where{.pc = 0, .symbol = {}, .offset = 0, .file = {}, .line = 0};
  popcorn::fault_info const fault{.kind = bad_kind, .fault_address = 0, .status = 0, .detail = 0};

  popcorn::fault_error const e{where, fault};
  CHECK(contains(e, "unknown(255)"));
}

TEST_CASE("budget_exceeded formats the budget and the symbol")
{
  popcorn::location const where{
      .pc = 0x0800'3000, .symbol = "loop", .offset = 0, .file = {}, .line = 0};
  popcorn::budget_exceeded const e{where, popcorn::icount{1'000'000}};

  CHECK(e.where().pc == where.pc);
  CHECK(e.budget().value == 1'000'000);
  CHECK(contains(e, "1000000"));
  CHECK(contains(e, "8003000"));
  CHECK(contains(e, "loop"));
}

TEST_CASE("budget_exceeded formats offset and file:line when present")
{
  popcorn::location const where{
      .pc = 0x0800'3010, .symbol = "loop", .offset = 0x4, .file = "loop.c", .line = 7};
  popcorn::budget_exceeded const e{where, popcorn::icount{42}};

  CHECK(e.where().offset == 0x4);
  CHECK(e.where().file == "loop.c");
  CHECK(e.where().line == 7);
  CHECK(contains(e, "+0x4"));
  CHECK(contains(e, "loop.c:7"));
}

TEST_CASE("model_error formats peripheral, register and offset")
{
  popcorn::model_error const e{"USART1", "ICR", 0x20};
  CHECK(e.peripheral() == "USART1");
  CHECK(e.reg() == "ICR");
  CHECK(e.offset() == 0x20);
  CHECK(contains(e, "USART1"));
  CHECK(contains(e, "ICR"));
  CHECK(contains(e, "20"));
}

TEST_CASE("unmodelled_config names register, field and value and is a model_error")
{
  popcorn::unmodelled_config const e{"MDF1", "SITF0CR", 0x80, "SITFMOD", 2, "Manchester mode"};
  CHECK(e.peripheral() == "MDF1");
  CHECK(e.reg() == "SITF0CR");
  CHECK(e.offset() == 0x80);
  CHECK(e.field() == "SITFMOD");
  CHECK(e.value() == 2);
  CHECK(std::string_view{e.what()} == "MDF1.SITF0CR.SITFMOD = 2 not modelled: Manchester mode");
  auto const* const base = dynamic_cast<popcorn::model_error const*>(&e);
  REQUIRE(base != nullptr);
  CHECK(base->reg() == "SITF0CR");
}

TEST_CASE("machine_running, machine_busy and not_implemented carry a plain message")
{
  CHECK(contains(popcorn::machine_running{"machine is running"}, "machine is running"));
  CHECK(contains(popcorn::machine_busy{"snapshot in progress"}, "snapshot in progress"));
  CHECK(contains(popcorn::not_implemented{"Machine::step"}, "Machine::step"));
}

TEST_CASE("the hierarchy derives every error from popcorn::error")
{
  CHECK(std::is_base_of_v<popcorn::error, popcorn::config_error>);
  CHECK(std::is_base_of_v<popcorn::error, popcorn::image_error>);
  CHECK(std::is_base_of_v<popcorn::error, popcorn::symbol_error>);
  CHECK(std::is_base_of_v<popcorn::error, popcorn::emulation_error>);
  CHECK(std::is_base_of_v<popcorn::emulation_error, popcorn::fault_error>);
  CHECK(std::is_base_of_v<popcorn::emulation_error, popcorn::budget_exceeded>);
  CHECK(std::is_base_of_v<popcorn::error, popcorn::model_error>);
  CHECK(std::is_base_of_v<popcorn::model_error, popcorn::unmodelled_config>);
  CHECK(std::is_base_of_v<popcorn::error, popcorn::machine_running>);
  CHECK(std::is_base_of_v<popcorn::error, popcorn::machine_busy>);
  CHECK(std::is_base_of_v<popcorn::error, popcorn::not_implemented>);
}

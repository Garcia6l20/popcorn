#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
auto env_path(char const* name) -> std::filesystem::path
{
  char const* const value = std::getenv(name);
  REQUIRE(value != nullptr);
  return value;
}

auto read_text(std::filesystem::path const& file) -> std::string
{
  std::ifstream input(file, std::ios::binary);
  REQUIRE(input);
  std::ostringstream text;
  text << input.rdbuf();
  return std::move(text).str();
}

auto canonical_json(std::string_view text) -> std::string
{
  namespace json         = reflex::serde::json;
  auto const       value = json::deserializer{text}.load<json::value>();
  std::string      out;
  json::serializer serializer{out};
  serializer.dump(value);
  return out;
}

auto toy_text() -> std::string
{
  return read_text(env_path("POPCORN_TOY_DESC"));
}

auto with_version(std::string text, std::string_view version) -> std::string
{
  std::string_view const current = "\"schema_version\": 1";
  auto const             at      = text.find(current);
  REQUIRE(at != std::string::npos);
  text.replace(at, current.size(), std::format("\"schema_version\": {}", version));
  return text;
}
}

TEST_CASE("toy description loads with its content")
{
  auto const toy = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  CHECK(toy.schema_version == popcorn::description_schema_version);
  CHECK(toy.part == "TOY1");
  CHECK(toy.core.cpu == "CM33");
  CHECK(toy.core.nvic_prio_bits == 3);
  CHECK(toy.core.revision == "r0p1");
  CHECK(toy.core.mpu_regions == 8);
  CHECK(toy.core.reset_clock_hz == 4'000'000);
  CHECK(toy.generator == "hand-written");
  CHECK(toy.sources.size() == 2);
  CHECK(toy.sources.at("rzone") == std::string(64, '1'));
  REQUIRE(toy.regions.size() == 2);
  CHECK(toy.regions[0].kind == popcorn::region_kind::rom);
  CHECK(
      toy.regions[0].acc == popcorn::region_access{.read = true, .write = false, .execute = true});
  CHECK_FALSE(toy.regions[0].secure);
  CHECK(toy.regions[1].start == 0x2000'0000);
  CHECK_FALSE(toy.regions[1].physical.has_value());
  REQUIRE(toy.peripherals.size() == 2);
  auto const& uart = toy.peripherals[0];
  CHECK(uart.base == 0x4000'1000);
  REQUIRE(uart.registers.size() == 2);
  CHECK(uart.registers[0].reset == 0xc0);
  CHECK(uart.registers[1].acc == popcorn::access::write_only);
  REQUIRE(uart.registers[1].fields.size() == 1);
  CHECK(uart.registers[1].fields[0].on_write == popcorn::write_effect::one_to_clear);
  REQUIRE(uart.cross_rules.size() == 1);
  CHECK(
      uart.cross_rules[0]
      == popcorn::cross_rule{"ICR", "TCCF", "ISR", "TC", popcorn::cross_effect::clear});
  CHECK(toy.peripherals[1].irqs == std::vector<std::uint16_t>{1});
  REQUIRE(toy.irqs.size() == 2);
  CHECK(toy.irqs[1] == popcorn::irq_desc{1, "TIMER1_IRQn"});
}

TEST_CASE("toy description re-serializes to an equal description")
{
  auto const toy  = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  auto const text = popcorn::dump_description(toy);
  CHECK(popcorn::parse_description(text, "dump") == toy);
  CHECK(canonical_json(text) == canonical_json(toy_text()));
}

TEST_CASE("optional members and every effect round-trip")
{
  popcorn::part_description description;
  description.schema_version = popcorn::description_schema_version;
  description.part           = "OPT";
  description.regions.push_back({
      "SRAM_S",
      0x3000'0000,
      0x4000,
      popcorn::region_kind::ram,
      {true, true, false},
      true,
      popcorn::address{0x2000'0000},
      std::string{"TIM1"},
      std::vector<popcorn::region_content>{{.offset = 0x10, .data = {0x00, 0x04}}}
  });
  popcorn::peripheral_desc sec;
  sec.name     = "SEC_TIM1";
  sec.base     = 0x5000'1000;
  sec.alias_of = "TIM1";
  sec.cross_rules.push_back({"CLR", "S", "SR", "F", popcorn::cross_effect::set});
  popcorn::register_desc alt;
  alt.name         = "CCMR1_INPUT";
  alt.width        = 32;
  alt.acc          = popcorn::access::read_write_once;
  alt.alternate_of = "CCMR1_OUTPUT";
  alt.fields.push_back(
      {"A", 0, 1, popcorn::access::write_once, popcorn::write_effect::zero_to_toggle,
       popcorn::read_effect::modify_external});
  alt.fields.push_back(
      {"B", 1, 1, popcorn::access::read_write, popcorn::write_effect::set,
       popcorn::read_effect::clear});
  sec.registers.push_back(alt);
  description.peripherals.push_back(sec);

  auto const text = popcorn::dump_description(description);
  CHECK(text.contains("\"alias_of\":\"TIM1\""));
  CHECK(text.contains("\"controller\":\"TIM1\""));
  CHECK(text.contains("\"content\":[{\"offset\":16,\"data\":[0,4]}]"));
  CHECK(text.contains("\"on_read\":\"modify_external\""));
  CHECK(popcorn::parse_description(text, "dump") == description);
}

TEST_CASE("GPIO alternate functions round-trip")
{
  popcorn::part_description description;
  description.schema_version = popcorn::description_schema_version;
  description.part           = "AF";
  popcorn::peripheral_desc port;
  port.name       = "GPIOA";
  port.alternates = std::vector<popcorn::alternate_desc>{
      {.pin = 6, .af = 2,  .signal = "tim3_ch1"},
      {.pin = 0, .af = 14, .signal = "tim2_etr"}
  };
  description.peripherals.push_back(port);

  auto const text = popcorn::dump_description(description);
  CHECK(text.contains("\"alternates\":[{\"pin\":6,\"af\":2,\"signal\":\"tim3_ch1\"}"));
  CHECK(popcorn::parse_description(text, "dump") == description);
}

TEST_CASE("register aliases round-trip")
{
  popcorn::part_description description;
  description.schema_version = popcorn::description_schema_version;
  description.part           = "ALIAS";
  popcorn::peripheral_desc timer;
  timer.name = "TIM1";
  popcorn::register_desc cr1;
  cr1.name    = "CR1";
  cr1.width   = 32;
  cr1.aliases = std::vector<std::string>{"TIM1_CR1"};
  timer.registers.push_back(cr1);
  description.peripherals.push_back(timer);

  auto const text = popcorn::dump_description(description);
  CHECK(text.contains("\"aliases\":[\"TIM1_CR1\"]"));
  CHECK(popcorn::parse_description(text, "dump") == description);
}

TEST_CASE("find_register resolves a name, then an alias, and null for neither")
{
  popcorn::peripheral_desc timer;
  timer.name = "TIM1";
  popcorn::register_desc cr1;
  cr1.name    = "CR1";
  cr1.aliases = std::vector<std::string>{"TIM1_CR1", "CONTROL"};
  popcorn::register_desc sr;
  sr.name         = "SR";
  sr.offset       = 0x10;
  timer.registers = {cr1, sr};

  CHECK(popcorn::find_register(timer, "CR1") == &timer.registers[0]);
  CHECK(popcorn::find_register(timer, "TIM1_CR1") == &timer.registers[0]);
  CHECK(popcorn::find_register(timer, "CONTROL") == &timer.registers[0]);
  CHECK(popcorn::find_register(timer, "SR") == &timer.registers[1]);
  CHECK(popcorn::find_register(timer, "TIM1_SR") == nullptr);
  CHECK(popcorn::find_register(timer, "") == nullptr);
}

TEST_CASE("absent optional members are omitted from the output")
{
  auto const text =
      popcorn::dump_description(popcorn::load_description(env_path("POPCORN_TOY_DESC")));
  CHECK_FALSE(text.contains("alias_of"));
  CHECK_FALSE(text.contains("alternates"));
  CHECK_FALSE(text.contains("alternate_of"));
  CHECK_FALSE(text.contains("aliases"));
  CHECK_FALSE(text.contains("physical"));
  CHECK_FALSE(text.contains("controller"));
  CHECK_FALSE(text.contains("content"));
  CHECK_FALSE(text.contains("null"));
}

TEST_CASE("dump stamps the current schema_version")
{
  popcorn::part_description description;
  REQUIRE(description.schema_version == 0);
  auto const parsed = popcorn::parse_description(popcorn::dump_description(description), "dump");
  CHECK(parsed.schema_version == popcorn::description_schema_version);
}

TEST_CASE("a missing schema_version is a config_error")
{
  auto       text = toy_text();
  auto const at   = text.find("\"schema_version\": 1,");
  REQUIRE(at != std::string::npos);
  text.erase(at, std::string_view{"\"schema_version\": 1,"}.size());
  for(auto const& variant : {text, std::string{text}.insert(1, "\"future_member\": 7,")})
  {
    try
    {
      static_cast<void>(popcorn::parse_description(variant, "toy.json"));
      FAIL("no config_error");
    }
    catch(popcorn::config_error const& e)
    {
      CHECK(e.key() == "schema_version");
      CHECK(std::string_view{e.what()}.contains("file has no schema_version"));
    }
  }
}

TEST_CASE("another schema_version is a config_error naming both versions")
{
  for(std::string_view const version : {"2", "0"})
  {
    CAPTURE(version);
    try
    {
      static_cast<void>(popcorn::parse_description(with_version(toy_text(), version), "toy.json"));
      FAIL("no config_error");
    }
    catch(popcorn::config_error const& e)
    {
      CHECK(e.file() == "toy.json");
      CHECK(e.key() == "schema_version");
      CHECK(
          std::string_view{e.what()}.contains(
              std::format("schema version 1, file has {}", version)));
    }
  }
}

TEST_CASE("a future schema_version with unknown members still names both versions")
{
  auto text = with_version(toy_text(), "2");
  text.insert(1, "\"future_member\": 7,");
  try
  {
    static_cast<void>(popcorn::parse_description(text, "toy.json"));
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& e)
  {
    CHECK(e.key() == "schema_version");
    CHECK(std::string_view{e.what()}.contains("schema version 1, file has 2"));
  }
}

TEST_CASE("malformed descriptions are config_errors")
{
  auto const toy = toy_text();
  for(std::string const& text :
      {std::string{"{"}, std::string{"[]"}, std::string{toy}.insert(1, "\"bogus\": 1,"),
       with_version(toy, "\"one\"")})
  {
    CHECK_THROWS_AS(
        static_cast<void>(popcorn::parse_description(text, "bad.json")), popcorn::config_error);
  }
  auto bad_enum = toy;
  bad_enum.replace(bad_enum.find("\"rom\""), 5, "\"flash\"");
  CHECK_THROWS_AS(
      static_cast<void>(popcorn::parse_description(bad_enum, "bad.json")), popcorn::config_error);
}

TEST_CASE("a missing file is a config_error naming it")
{
  try
  {
    static_cast<void>(popcorn::load_description("/nonexistent/popcorn/desc.json"));
    FAIL("no config_error");
  }
  catch(popcorn::config_error const& e)
  {
    CHECK(e.file() == "/nonexistent/popcorn/desc.json");
  }
}

TEST_CASE("committed JSON Schema matches the one derived from the aggregates")
{
  auto const committed = read_text(env_path("POPCORN_DESC_SCHEMA"));
  INFO("regenerate with: uv run pcons run schema");
  CHECK(canonical_json(committed) == canonical_json(popcorn::description_json_schema()));
}

TEST_CASE("JSON Schema defines each enum once under its C++ name and refers to it")
{
  namespace json = reflex::serde::json;
  auto document  = json::deserializer{popcorn::description_json_schema()}.load<json::value>();
  auto defs      = std::move(document.as_object()["$defs"]);
  document.as_object().erase("$defs");

  auto dumped = [](json::value const& value) {
    std::string      out;
    json::serializer serializer{out};
    serializer.dump(value);
    return out;
  };
  CHECK_FALSE(dumped(document).contains("\"enum\""));
  CHECK(
      canonical_json(dumped(defs.as_object()["region_kind"]))
      == canonical_json(R"({"type": "string", "enum": ["rom", "ram"]})"));
  auto& field = defs.as_object()["field_desc"].as_object()["properties"].as_object();
  CHECK(canonical_json(dumped(field["acc"])) == canonical_json(R"({"$ref": "#/$defs/access"})"));
  CHECK(
      canonical_json(dumped(field["on_write"]))
      == canonical_json(R"({"$ref": "#/$defs/write_effect"})"));
  auto& properties = defs.as_object()["register_desc"].as_object()["properties"].as_object();
  CHECK(
      canonical_json(dumped(properties["acc"])) == canonical_json(R"({"$ref": "#/$defs/access"})"));
}

TEST_CASE("flat register ids number registers peripheral by peripheral")
{
  auto const toy = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  CHECK(popcorn::flat_register_id(toy, {.peripheral = 0, .reg = 1}) == 1);
  CHECK(popcorn::flat_register_id(toy, {.peripheral = 1, .reg = 0}) == 2);
  CHECK(popcorn::register_at(toy, 2) == popcorn::register_ref{.peripheral = 1, .reg = 0});
  CHECK(popcorn::register_at(toy, 0) == popcorn::register_ref{.peripheral = 0, .reg = 0});
  CHECK_FALSE(popcorn::register_at(toy, 3).has_value());
}

TEST_CASE("toy description digest matches the value computed independently from its dump")
{
  auto const toy = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  CHECK(
      popcorn::description_digest(toy)
      == "b811faf7a695fa3e258a09d9bfbd353d3d8cf0e574b883684e85cdb4d1c974e3");
}

TEST_CASE("digest is 64 lower-case hex characters")
{
  auto const toy    = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  auto const digest = popcorn::description_digest(toy);
  CHECK(digest.size() == 64);
  CHECK(std::ranges::all_of(digest, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }));
}

TEST_CASE("digest is stable across repeated calls")
{
  auto const toy = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  CHECK(popcorn::description_digest(toy) == popcorn::description_digest(toy));
}

TEST_CASE("descriptions differing in one register reset value have different digests")
{
  auto changed = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  changed.peripherals[0].registers[0].reset += 1;
  auto const toy = popcorn::load_description(env_path("POPCORN_TOY_DESC"));
  CHECK(popcorn::description_digest(toy) != popcorn::description_digest(changed));
}

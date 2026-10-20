#include "dfilter_chain.hpp"
#include "sigma_delta.hpp"

#include <doctest/doctest.h>
#include <reflex/serde/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace stm32 = popcorn::periph::stm32;
namespace json  = reflex::serde::json;

auto vector_dir() -> std::filesystem::path
{
  char const* const value = std::getenv("POPCORN_MDF_VECTORS");
  REQUIRE(value != nullptr);
  return value;
}

auto load(std::string_view file) -> json::value
{
  std::ifstream input(vector_dir() / file, std::ios::binary);
  REQUIRE(input);
  std::ostringstream text;
  text << input.rdbuf();
  return json::deserializer{std::move(text).str()}.load<json::value>();
}

auto integer(json::value const& value) -> std::int64_t
{
  return static_cast<std::int64_t>(value.as<json::number>());
}

auto member(json::value const& object, std::string_view key) -> json::value const&
{
  auto const found = object.at(key);
  REQUIRE(found.has_value());
  return *found;
}

auto run_length(json::value const& pairs) -> std::vector<std::int64_t>
{
  std::vector<std::int64_t> out;
  for(auto const& pair : pairs.as_array())
  {
    out.insert(out.end(), static_cast<std::size_t>(integer(pair[1])), integer(pair[0]));
  }
  return out;
}

auto bits_of(json::value const& bits) -> std::vector<bool>
{
  if(auto const runs = bits.at("runs"))
  {
    std::vector<bool> out;
    for(auto const value : run_length(*runs))
    {
      out.push_back(value != 0);
    }
    return out;
  }
  auto const& hex   = member(bits, "hex").as<json::string>();
  auto const  count = static_cast<std::size_t>(integer(member(bits, "count")));
  REQUIRE(hex.size() * 4 >= count);
  std::vector<bool> out(count);
  for(std::size_t i = 0; i < count; ++i)
  {
    auto const byte = std::stoul(hex.substr(i / 8 * 2, 2), nullptr, 16);
    out[i]          = (byte & (0x80u >> i % 8)) != 0;
  }
  return out;
}

auto order_of(std::string_view order) -> stm32::cic_order
{
  if(order == "fastsinc")
  {
    return stm32::cic_order::fastsinc;
  }
  REQUIRE(order.size() == 1);
  REQUIRE(order[0] >= '1');
  REQUIRE(order[0] <= '5');
  return static_cast<stm32::cic_order>(order[0] - '0');
}

auto divider_of(std::int64_t ratio) -> stm32::int_divider
{
  switch(ratio)
  {
    case 128:
      return stm32::int_divider::by_128;
    case 32:
      return stm32::int_divider::by_32;
    case 4:
      return stm32::int_divider::by_4;
    case 1:
      return stm32::int_divider::by_1;
  }
  FAIL("INT ratio " << ratio);
  return {};
}

auto optional_byte(json::value const& value) -> std::optional<std::uint8_t>
{
  if(value.is_null())
  {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(integer(value));
}

auto config_of(json::value const& config) -> stm32::chain_config
{
  return {
      .order          = order_of(member(config, "order").as<json::string>()),
      .decimation     = static_cast<std::uint16_t>(integer(member(config, "decimation"))),
      .offset         = static_cast<std::int32_t>(integer(member(config, "offset"))),
      .scale          = static_cast<std::uint8_t>(integer(member(config, "scale"))),
      .rsflt          = member(config, "rsflt").as<json::boolean>(),
      .rsflt_decimate = member(config, "rsflt_decimate").as<json::boolean>(),
      .hpf            = optional_byte(member(config, "hpf")),
      .int_value      = optional_byte(member(config, "int_value")),
      .int_div        = divider_of(integer(member(config, "int_div"))),
  };
}

struct chain_output
{
  std::vector<std::int64_t> pcm;
  std::vector<std::int64_t> saturated;
};

auto run_chain(json::value const& config, std::vector<bool> const& bits) -> chain_output
{
  auto const         chain   = config_of(config);
  auto const         skip    = static_cast<std::size_t>(integer(member(config, "skip")));
  auto               discard = integer(member(config, "discard"));
  bool               carried = false;
  chain_output       out;
  stm32::chain_state state{};
  for(std::size_t i = skip; i < bits.size(); ++i)
  {
    auto sample = stm32::feed(state, chain, bits[i] ? 1 : -1);
    if(not sample)
    {
      continue;
    }
    if(discard > 0)
    {
      --discard;
      carried = carried or sample->saturated;
      continue;
    }
    sample->saturated = sample->saturated or std::exchange(carried, false);
    if(auto const kept = stm32::integrate(state.integ, chain, *sample))
    {
      out.pcm.push_back(kept->pcm);
      out.saturated.push_back(kept->saturated ? 1 : 0);
    }
  }
  return out;
}

auto first_difference(std::vector<std::int64_t> const& got, std::vector<std::int64_t> const& want)
    -> std::size_t
{
  auto const [at, _] = std::ranges::mismatch(got, want);
  return static_cast<std::size_t>(at - got.begin());
}

void check_chain(std::string_view name)
{
  auto const document = load(std::string{"chain_"} + std::string{name} + ".json");
  CHECK(member(document, "name").as<json::string>() == name);
  std::size_t index = 0;
  for(auto const& run : member(document, "runs").as_array())
  {
    CAPTURE(index);
    auto const got  = run_chain(member(run, "config"), bits_of(member(run, "bits")));
    auto const pcm  = run_length(member(run, "pcm"));
    auto const sats = run_length(member(run, "saturated"));
    CHECK(got.pcm.size() == pcm.size());
    CHECK(first_difference(got.pcm, pcm) == pcm.size());
    CHECK(got.saturated.size() == sats.size());
    CHECK(first_difference(got.saturated, sats) == sats.size());
    ++index;
  }
  CHECK(index > 0);
}

void check_modulator(std::string_view name)
{
  auto const  document  = load(std::string{"modulator_"} + std::string{name} + ".json");
  auto const& modulator = member(document, "modulator");
  auto const& pcm16     = member(modulator, "pcm16").as_array();
  auto const& held      = member(modulator, "bits_per_sample").as_array();
  REQUIRE(pcm16.size() == held.size());
  stm32::modulator_state state{};
  std::vector<bool>      got;
  for(std::size_t i = 0; i < pcm16.size(); ++i)
  {
    for(std::int64_t n = 0; n < integer(held[i]); ++n)
    {
      got.push_back(stm32::modulate(state, static_cast<std::int32_t>(integer(pcm16[i]))));
    }
  }
  auto const want    = bits_of(member(modulator, "bits"));
  auto const [at, _] = std::ranges::mismatch(got, want);
  CHECK(got.size() == want.size());
  CHECK(static_cast<std::size_t>(at - got.begin()) == want.size());
}

constexpr std::string_view chain_vectors[]{
    "d2",           "d512",       "discard_skip", "fastsinc",   "hpf0",   "hpf1",
    "hpf2",         "hpf3",       "int",          "int_bypass", "offset", "pdm_16k",
    "pdm_16k_loud", "rsflt_dec1", "sat",          "scale_all",  "sinc1",  "sinc2",
    "sinc3",        "sinc4",      "sinc5",        "wrap",
};

constexpr std::string_view modulator_vectors[]{"16k", "32k", "44k1", "64k", "overload"};
}

TEST_CASE("every committed MDF vector has a case here")
{
  std::set<std::string> expected;
  for(auto const name : chain_vectors)
  {
    expected.insert(std::string{"chain_"} + std::string{name} + ".json");
  }
  for(auto const name : modulator_vectors)
  {
    expected.insert(std::string{"modulator_"} + std::string{name} + ".json");
  }
  std::set<std::string> found;
  for(auto const& entry : std::filesystem::directory_iterator(vector_dir()))
  {
    if(entry.path().extension() == ".json")
    {
      found.insert(entry.path().filename().string());
    }
  }
  CHECK(found == expected);
}

TEST_CASE("SCALE codes give the gains of RM0456 Table 377")
{
  CHECK(stm32::scale_of(0x00)->mult == 1);
  CHECK(stm32::scale_of(0x00)->shift == 0);
  CHECK(stm32::scale_of(0x01)->mult == 3);
  CHECK(stm32::scale_of(0x01)->shift == -1);
  CHECK(stm32::scale_of(0x18)->shift == 12);
  CHECK(stm32::scale_of(0x20)->shift == -8);
  CHECK(stm32::scale_of(0x2C)->shift == -2);
  CHECK(stm32::scale_of(0x2F)->mult == 3);
  CHECK(stm32::scale_of(0x2F)->shift == -2);
  for(auto const reserved : {0x19, 0x1F, 0x30, 0x3F})
  {
    CAPTURE(reserved);
    CHECK_FALSE(stm32::scale_of(static_cast<std::uint8_t>(reserved)).has_value());
  }
}

TEST_CASE("chain d2")
{
  check_chain("d2");
}

TEST_CASE("chain d512")
{
  check_chain("d512");
}

TEST_CASE("chain discard_skip")
{
  check_chain("discard_skip");
}

TEST_CASE("chain fastsinc")
{
  check_chain("fastsinc");
}

TEST_CASE("chain hpf0")
{
  check_chain("hpf0");
}

TEST_CASE("chain hpf1")
{
  check_chain("hpf1");
}

TEST_CASE("chain hpf2")
{
  check_chain("hpf2");
}

TEST_CASE("chain hpf3")
{
  check_chain("hpf3");
}

TEST_CASE("chain int")
{
  check_chain("int");
}

TEST_CASE("chain int_bypass")
{
  check_chain("int_bypass");
}

TEST_CASE("chain offset")
{
  check_chain("offset");
}

TEST_CASE("chain pdm_16k")
{
  check_chain("pdm_16k");
}

TEST_CASE("chain pdm_16k_loud")
{
  check_chain("pdm_16k_loud");
}

TEST_CASE("chain rsflt_dec1")
{
  check_chain("rsflt_dec1");
}

TEST_CASE("chain sat")
{
  check_chain("sat");
}

TEST_CASE("chain scale_all")
{
  check_chain("scale_all");
}

TEST_CASE("chain sinc1")
{
  check_chain("sinc1");
}

TEST_CASE("chain sinc2")
{
  check_chain("sinc2");
}

TEST_CASE("chain sinc3")
{
  check_chain("sinc3");
}

TEST_CASE("chain sinc4")
{
  check_chain("sinc4");
}

TEST_CASE("chain sinc5")
{
  check_chain("sinc5");
}

TEST_CASE("chain wrap, one second of ones through Sinc5 D 32")
{
  check_chain("wrap");
}

TEST_CASE("modulator 16k")
{
  check_modulator("16k");
}

TEST_CASE("modulator 32k")
{
  check_modulator("32k");
}

TEST_CASE("modulator 44k1")
{
  check_modulator("44k1");
}

TEST_CASE("modulator 64k")
{
  check_modulator("64k");
}

TEST_CASE("modulator overload")
{
  check_modulator("overload");
}

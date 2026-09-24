#include <popcorn/core/description.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <print>
#include <ranges>
#include <vector>

namespace
{
constexpr std::size_t peripheral_count         = 200;
constexpr std::size_t registers_per_peripheral = 25;
constexpr std::size_t fields_per_register      = 10;
constexpr std::size_t runs                     = 20;

auto synthetic_description() -> popcorn::mcu_description
{
  popcorn::mcu_description description;
  description.schema_version = popcorn::description_schema_version;
  description.part           = "SYNTH5K";
  description.generator      = "bench-description";
  description.sources        = {
      {"svd",   std::string(64, 'a')},
      {"rzone", std::string(64, 'b')}
  };
  description.core.cpu      = "CM33";
  description.core.fpu      = true;
  description.core.num_irqs = 125;
  for(auto const p : std::views::iota(std::size_t{0}, peripheral_count))
  {
    popcorn::peripheral_desc peripheral;
    peripheral.name       = std::format("PERIPH{}", p);
    peripheral.ip         = "usart";
    peripheral.ip_version = "sci3_v1_2_Cube";
    peripheral.base       = static_cast<popcorn::address>(0x4000'0000 + p * 0x400);
    peripheral.size       = 0x400;
    peripheral.irqs       = {static_cast<std::uint16_t>(p % 125)};
    for(auto const r : std::views::iota(std::size_t{0}, registers_per_peripheral))
    {
      popcorn::register_desc reg;
      reg.name       = std::format("PERIPH{}_REG{}", p, r);
      reg.offset     = static_cast<std::uint32_t>(r * 4);
      reg.width      = 32;
      reg.reset      = 0x0000'00c0;
      reg.reset_mask = 0xffff'ffff;
      reg.acc        = popcorn::access::read_write;
      for(auto const f : std::views::iota(std::size_t{0}, fields_per_register))
      {
        reg.fields.push_back(
            {std::format("FIELD{}", f), static_cast<std::uint8_t>(f * 3), 3,
             popcorn::access::read_write, popcorn::write_effect::modify,
             popcorn::read_effect::none});
      }
      peripheral.registers.push_back(std::move(reg));
    }
    peripheral.cross_rules.push_back(
        {"PERIPH_REG1", "FIELD0", "PERIPH_REG0", "FIELD0", popcorn::cross_effect::clear});
    description.peripherals.push_back(std::move(peripheral));
  }
  return description;
}

auto percentile(std::vector<double> sorted, double fraction) -> double
{
  std::ranges::sort(sorted);
  auto const index =
      static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1) + 0.5);
  return sorted[index];
}
}

auto main() -> int
{
  auto const description = synthetic_description();
  auto const text        = popcorn::dump_description(description);
  std::println(
      "registers {}, fields {}, JSON {} bytes", peripheral_count * registers_per_peripheral,
      peripheral_count * registers_per_peripheral * fields_per_register, text.size());

  std::vector<double> samples;
  for(std::size_t run = 0; run < runs; ++run)
  {
    auto const start  = std::chrono::steady_clock::now();
    auto const parsed = popcorn::parse_description(text, "synthetic");
    auto const stop   = std::chrono::steady_clock::now();
    if(parsed != description)
    {
      std::println("parse mismatch");
      return 1;
    }
    samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
  }
  std::println(
      "parse over {} runs: median {:.1f} ms, p95 {:.1f} ms, min {:.1f} ms, max {:.1f} ms", runs,
      percentile(samples, 0.5), percentile(samples, 0.95), std::ranges::min(samples),
      std::ranges::max(samples));
}

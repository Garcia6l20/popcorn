#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/obs/pctr.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace popcorn::obs
{
auto pctr_writer_for(core::machine const& target, std::filesystem::path path)
    -> std::unique_ptr<pctr_writer>
{
  pctr_names  names;
  auto const& description = target.description();
  for(std::uint32_t id = 0;; ++id)
  {
    auto const found = register_at(description, id);
    if(not found)
    {
      break;
    }
    auto const& peripheral = description.peripherals[found->peripheral];
    auto const& reg        = peripheral.registers[found->reg];
    auto        name       = peripheral.name + "." + reg.name;
    names.register_ranges.push_back(
        {.start = peripheral.base + reg.offset,
         .size  = static_cast<std::uint32_t>((reg.width + 7) / 8),
         .name  = name});
    names.registers.push_back(std::move(name));
  }
  for(auto const& loaded : target.observed_images())
  {
    for(auto const& symbol : loaded.symbols)
    {
      names.symbols.push_back(symbol.name);
      if(symbol.size != 0)
      {
        names.symbol_ranges.push_back(
            {.start = symbol.value, .size = symbol.size, .name = symbol.name});
      }
    }
  }
  return std::make_unique<pctr_writer>(
      std::move(path), target.virtual_clock().now(), std::move(names));
}
}

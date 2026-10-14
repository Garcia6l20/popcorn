#include "register_layer.hpp"

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>

namespace popcorn::periph
{
namespace
{
constexpr auto low_bits(std::uint32_t width) noexcept -> std::uint32_t
{
  return width >= 32 ? std::numeric_limits<std::uint32_t>::max() : (std::uint32_t{1} << width) - 1;
}

constexpr auto lane_mask(std::uint32_t offset, std::uint32_t size) noexcept -> std::uint32_t
{
  return low_bits(8 * size) << (8 * (offset % 4));
}

constexpr auto readable(access acc) noexcept -> bool
{
  switch(acc)
  {
    case access::read_only:
    case access::read_write:
    case access::read_write_once:
      return true;
    case access::write_only:
    case access::write_once:
      return false;
  }
  return false;
}

constexpr auto writable(access acc) noexcept -> bool
{
  return acc != access::read_only;
}

constexpr auto once(access acc) noexcept -> bool
{
  return acc == access::write_once or acc == access::read_write_once;
}

void add_bits(
    word_rules&   rules,
    std::uint32_t mask,
    access        acc,
    write_effect  on_write,
    read_effect   on_read)
{
  if(readable(acc))
  {
    rules.readable |= mask;
  }
  if(writable(acc))
  {
    rules.writable |= mask;
    rules.on_write[effect_index(on_write)] |= mask;
  }
  if(once(acc))
  {
    rules.write_once |= mask;
  }
  switch(on_read)
  {
    case read_effect::clear:
      rules.read_clear |= mask;
      break;
    case read_effect::set:
      rules.read_set |= mask;
      break;
    case read_effect::none:
    case read_effect::modify:
    case read_effect::modify_external:
      break;
  }
}

struct placed_register
{
  register_desc const* desc{};
  std::uint32_t        word{};
  std::uint32_t        shift{};
};

auto apply(write_effect effect, std::uint32_t value, std::uint32_t written, std::uint32_t mask)
    -> std::uint32_t
{
  switch(effect)
  {
    case write_effect::modify:
      return (value & ~mask) | (written & mask);
    case write_effect::one_to_clear:
      return value & ~(written & mask);
    case write_effect::one_to_set:
      return value | (written & mask);
    case write_effect::one_to_toggle:
      return value ^ (written & mask);
    case write_effect::zero_to_clear:
      return value & (written | ~mask);
    case write_effect::zero_to_set:
      return value | (~written & mask);
    case write_effect::zero_to_toggle:
      return value ^ (~written & mask);
    case write_effect::clear:
      return value & ~mask;
    case write_effect::set:
      return value | mask;
  }
  return value;
}

auto locked(layer_state const& state, std::uint32_t word) noexcept -> bool
{
  return word / 32 < state.write_locked.size()
     and (state.write_locked[word / 32] >> (word % 32) & 1u) != 0;
}
}

register_layout::register_layout(peripheral_desc const& desc) : desc_{&desc}
{
  auto const words = (desc.size + 3) / 4;
  rule_of_word_.assign(words, 0);
  auto const fail = [&desc](std::string const& what, std::string expected) {
    throw config_error(
        desc.name, 0, std::format("peripherals.{}.{}", desc.name, what), std::move(expected));
  };

  std::map<std::string, placed_register, std::less<>> placed;
  std::set<std::uint32_t>                             offsets;
  for(auto const& reg : desc.registers)
  {
    auto const shift  = 8 * (reg.offset % 4);
    auto const inside = reg.width != 0
                    and reg.width % 8 == 0
                    and shift + reg.width <= 32
                    and std::uint64_t{reg.offset} + reg.width / 8 <= desc.size;
    if(not inside)
    {
      fail(
          "registers." + reg.name,
          "a register of 8, 16 or 32 bits inside one word of the peripheral");
    }
    if(not placed
               .emplace(
                   reg.name, placed_register{.desc = &reg, .word = reg.offset / 4, .shift = shift})
               .second)
    {
      fail("registers." + reg.name, "one register of that name");
    }
    if(not reg.alternate_of and not offsets.insert(reg.offset).second)
    {
      fail("registers." + reg.name, "one register per offset, others naming it in alternate_of");
    }
  }
  std::map<std::string, placed_register const*, std::less<>> aliased;
  for(auto const& reg : desc.registers | std::views::filter([](register_desc const& reg) {
                          return reg.aliases.has_value();
                        }))
  {
    for(auto const& alias : *reg.aliases)
    {
      if(placed.contains(alias) or not aliased.emplace(alias, &placed.at(reg.name)).second)
      {
        fail(
            "registers." + reg.name + ".aliases",
            "names no other register of the peripheral uses as its name or alias");
      }
    }
  }
  auto const placed_named = [&](std::string_view name) -> placed_register const* {
    if(auto const found = placed.find(name); found != placed.end())
    {
      return &found->second;
    }
    auto const found = aliased.find(name);
    return found == aliased.end() ? nullptr : found->second;
  };

  auto const rules_for = [this, &fail](std::uint32_t word) -> word_rules& {
    if(rule_of_word_[word] == 0)
    {
      if(rules_.size() >= std::numeric_limits<std::uint16_t>::max())
      {
        fail("registers", "at most 65535 storage words with registers");
      }
      rules_.emplace_back();
      rule_of_word_[word] = static_cast<std::uint16_t>(rules_.size());
    }
    return rules_[rule_of_word_[word] - 1];
  };

  for(auto const& [name, at] : placed)
  {
    auto const& reg  = *at.desc;
    auto        word = at.word;
    if(reg.alternate_of)
    {
      auto const* primary = placed_named(*reg.alternate_of);
      if(primary == nullptr)
      {
        fail("registers." + reg.name + ".alternate_of", "the name of a register of the peripheral");
      }
      word = primary->word;
    }
    word_rules own{};
    if(not reg.alternate_of)
    {
      own.reset = (reg.reset & reg.reset_mask & low_bits(reg.width)) << at.shift;
    }
    std::uint32_t covered = 0;
    for(auto const& field : reg.fields)
    {
      if(field.width == 0 or field.lsb + field.width > reg.width)
      {
        fail("registers." + reg.name + ".fields." + field.name, "a field inside its register");
      }
      auto const mask = (low_bits(field.width) << field.lsb) << at.shift;
      covered |= mask;
      add_bits(own, mask, field.acc, field.on_write, field.on_read);
    }
    add_bits(
        own, (low_bits(reg.width) << at.shift) & ~covered, reg.acc, write_effect::modify,
        read_effect::none);

    auto&      rules   = rules_for(word);
    auto const overlap = rules.writable & own.writable;
    for(auto const [mine, theirs] : std::views::zip(own.on_write, rules.on_write))
    {
      if(((mine ^ theirs) & overlap) != 0)
      {
        fail(
            "registers." + reg.name,
            "the write effects of the register it shares bits with, as an alternate view");
      }
    }
    rules.reset |= own.reset;
    rules.readable |= own.readable;
    rules.writable |= own.writable;
    rules.write_once |= own.write_once;
    rules.read_clear |= own.read_clear;
    rules.read_set |= own.read_set;
    for(auto const [into, from] : std::views::zip(rules.on_write, own.on_write))
    {
      into |= from;
    }
  }
  write_once_ =
      std::ranges::any_of(rules_, [](word_rules const& rules) { return rules.write_once != 0; });

  auto const field_bits = [&](std::string const& reg_name, std::string const& field_name,
                              std::string const& what) -> std::pair<std::uint32_t, std::uint32_t> {
    auto const* reg = placed_named(reg_name);
    if(reg != nullptr)
    {
      auto const& fields = reg->desc->fields;
      auto const  field  = std::ranges::find(fields, field_name, &field_desc::name);
      if(field != fields.end())
      {
        auto const word =
            reg->desc->alternate_of ? placed_named(*reg->desc->alternate_of)->word : reg->word;
        return {word, (low_bits(field->width) << field->lsb) << reg->shift};
      }
    }
    fail(what, "a register and field of the peripheral");
    return {};
  };
  for(auto const& rule : desc.cross_rules)
  {
    auto const what                       = std::format("cross_rules.{}.{}", rule.reg, rule.field);
    auto const [source_word, source_mask] = field_bits(rule.reg, rule.field, what);
    auto const [target_word, target_mask] = field_bits(rule.target_reg, rule.target_field, what);
    crosses_.push_back(
        {.source_word = source_word,
         .source_mask = source_mask,
         .target_word = target_word,
         .target_mask = target_mask,
         .effect      = rule.effect});
  }
  std::ranges::stable_sort(crosses_, {}, &cross_target::source_word);
}

auto register_layout::words() const noexcept -> std::uint32_t
{
  return static_cast<std::uint32_t>(rule_of_word_.size());
}

auto register_layout::rules(std::uint32_t word) const noexcept -> word_rules const&
{
  static constexpr word_rules none{};
  auto const                  index = rule_of_word_[word];
  return index == 0 ? none : rules_[index - 1];
}

auto register_layout::crosses(std::uint32_t word) const -> std::span<cross_target const>
{
  if(crosses_.empty())
  {
    return {};
  }
  auto const range = std::ranges::equal_range(crosses_, word, {}, &cross_target::source_word);
  return {range.begin(), range.end()};
}

auto register_layout::word_of(std::string_view name) const -> std::optional<std::uint32_t>
{
  auto const* reg = find_register(*desc_, name);
  if(reg == nullptr)
  {
    return std::nullopt;
  }
  if(reg->alternate_of)
  {
    return word_of(*reg->alternate_of);
  }
  return reg->offset / 4;
}

auto register_layout::find(std::uint32_t word) const noexcept -> word_rules const*
{
  auto const index = rule_of_word_[word];
  return index == 0 ? nullptr : &rules_[index - 1];
}

auto register_layout::rule_count() const noexcept -> std::size_t
{
  return rules_.size();
}

auto register_layout::has_register(std::uint32_t word) const noexcept -> bool
{
  return rule_of_word_[word] != 0;
}

auto register_layout::has_write_once() const noexcept -> bool
{
  return write_once_;
}

auto register_layout::footprint() const noexcept -> std::size_t
{
  return rule_of_word_.size() * sizeof(std::uint16_t)
       + rules_.size() * sizeof(word_rules)
       + crosses_.size() * sizeof(cross_target);
}

auto layer_read(
    register_layout const& layout,
    register_file&         regs,
    std::uint32_t          offset,
    std::uint8_t           size) -> std::uint32_t
{
  return layer_read(layout.rules(offset / 4), regs, offset, size);
}

auto layer_read(
    word_rules const& rules,
    register_file&    regs,
    std::uint32_t     offset,
    std::uint8_t      size) -> std::uint32_t
{
  auto const word  = offset / 4;
  auto const lane  = lane_mask(offset, size);
  auto const old   = regs.peek(word * 4);
  auto const value = (old & rules.readable & lane) >> (8 * (offset % 4));
  auto const drop  = rules.read_clear & lane;
  auto const raise = rules.read_set & lane;
  if((drop | raise) != 0)
  {
    auto const after = (old & ~drop) | raise;
    if(after != old)
    {
      regs.poke(word * 4, after);
    }
  }
  return value;
}

void layer_write(
    register_layout const& layout,
    register_file&         regs,
    layer_state&           state,
    std::uint32_t          offset,
    std::uint8_t           size,
    std::uint32_t          value)
{
  layer_write(layout, layout.rules(offset / 4), regs, state, offset, size, value);
}

void layer_write(
    register_layout const& layout,
    word_rules const&      rules,
    register_file&         regs,
    layer_state&           state,
    std::uint32_t          offset,
    std::uint8_t           size,
    std::uint32_t          value)
{
  constexpr auto modify  = effect_index(write_effect::modify);
  auto const     word    = offset / 4;
  auto const     lane    = lane_mask(offset, size);
  auto const     written = (value << (8 * (offset % 4))) & lane;
  auto const     open    = rules.writable & ~(locked(state, word) ? rules.write_once : 0u) & lane;
  auto           stored  = regs.peek(word * 4);
  if((open & ~rules.on_write[modify]) == 0)
  {
    stored = (stored & ~open) | (written & open);
  }
  else
  {
    std::size_t position = 0;
    template for(constexpr auto e :
                 std::define_static_array(std::meta::enumerators_of(^^write_effect)))
    {
      if(auto const mask = rules.on_write[position] & open; mask != 0)
      {
        stored = apply([:e:], stored, written, mask);
      }
      ++position;
    }
  }
  regs.poke(word * 4, stored);
  if((rules.write_once & lane) != 0)
  {
    auto const words = (layout.words() + 31) / 32;
    if(state.write_locked.size() < words)
    {
      state.write_locked.resize(words);
    }
    state.write_locked[word / 32] |= std::uint32_t{1} << (word % 32);
  }
  for(auto const& cross : layout.crosses(word))
  {
    if((written & cross.source_mask) == 0)
    {
      continue;
    }
    auto const target = regs.peek(cross.target_word * 4);
    switch(cross.effect)
    {
      case cross_effect::clear:
        regs.poke(cross.target_word * 4, target & ~cross.target_mask);
        break;
      case cross_effect::set:
        regs.poke(cross.target_word * 4, target | cross.target_mask);
        break;
    }
  }
}

void layer_reset(
    register_layout const&        layout,
    register_file&                regs,
    layer_state&                  state,
    std::span<std::uint8_t const> claimed)
{
  for(std::uint32_t word = 0; word < layout.words(); ++word)
  {
    if(claimed[word] == 0)
    {
      regs.poke(word * 4, layout.rules(word).reset);
    }
  }
  if(layout.has_write_once())
  {
    state.write_locked.assign((layout.words() + 31) / 32, 0);
  }
  else
  {
    state.write_locked.clear();
  }
}
}

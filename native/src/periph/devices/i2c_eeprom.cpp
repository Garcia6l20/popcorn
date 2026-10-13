#include "image_file.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <utility>
#include <vector>

namespace popcorn::periph::devices
{
namespace
{
enum class phase : std::uint8_t
{
  idle,
  word_address,
  writing,
  reading,
};

struct saved_page
{
  std::uint32_t          index{};
  std::vector<std::byte> bytes;

  friend auto operator==(saved_page const&, saved_page const&) -> bool = default;
};

struct eeprom_state
{
  std::uint32_t           size{};
  std::uint64_t           image_digest{};
  std::uint64_t           busy_until{};
  std::uint8_t            step{};
  std::uint8_t            address_count{};
  std::uint32_t           word_address{};
  std::uint32_t           pointer{};
  std::uint32_t           page_base{};
  std::uint32_t           data_count{};
  std::vector<std::byte>  buffer;
  std::vector<saved_page> pages;

  friend auto operator==(eeprom_state const&, eeprom_state const&) -> bool = default;
};

auto checked(eeprom_24xx_options options) -> eeprom_24xx_options
{
  auto const fail = [](char const* key, std::string expected) {
    throw config_error("eeprom_24xx", 0, key, std::move(expected));
  };
  if(options.address >= 0x80)
  {
    fail("address", std::format("a 7-bit address, not {:#x}", options.address));
  }
  if(options.address_bytes != 1 and options.address_bytes != 2)
  {
    fail("address_bytes", std::format("1 or 2, not {}", options.address_bytes));
  }
  if(options.page_size == 0 or not std::has_single_bit(options.page_size))
  {
    fail("page_size", std::format("a power of two, not {}", options.page_size));
  }
  auto const limit = std::uint32_t{1} << (8u * options.address_bytes);
  if(options.size == 0 or options.size % options.page_size != 0 or options.size > limit)
  {
    fail(
        "size",
        std::format(
            "a non-zero multiple of the page size up to {} bytes, not {}", limit, options.size));
  }
  return options;
}
}

struct eeprom_24xx::impl
{
  eeprom_24xx_options    options;
  std::vector<std::byte> initial;
  std::vector<std::byte> array;
  std::uint64_t          digest;
  eeprom_state           state{};

  explicit impl(eeprom_24xx_options opts)
      : options{checked(std::move(opts))},
        initial{read_image(options.image, options.size, "EEPROM")}, array{initial},
        digest{digest_of(initial)}
  {}

  [[nodiscard]] auto step() const -> phase
  {
    return static_cast<phase>(state.step);
  }

  void enter(phase next)
  {
    state.step = static_cast<std::uint8_t>(next);
  }

  [[nodiscard]] auto busy(vtime at) const -> bool
  {
    return at.ps < state.busy_until;
  }

  [[nodiscard]] auto page_differs(std::uint32_t page) const -> bool
  {
    auto const at = std::size_t{page} * options.page_size;
    return std::memcmp(array.data() + at, initial.data() + at, options.page_size) != 0;
  }

  void open_page()
  {
    state.page_base  = state.pointer & ~(options.page_size - 1);
    state.data_count = 0;
    auto const first = array.begin() + static_cast<std::ptrdiff_t>(state.page_base);
    state.buffer.assign(first, first + options.page_size);
  }

  [[nodiscard]] auto in_page(std::uint32_t count) const -> std::uint32_t
  {
    return (state.pointer - state.page_base + count) % options.page_size;
  }

  void commit(vtime at)
  {
    std::ranges::copy(state.buffer, array.begin() + static_cast<std::ptrdiff_t>(state.page_base));
    state.pointer    = state.page_base + in_page(state.data_count);
    state.busy_until = at.ps + static_cast<std::uint64_t>(options.write_cycle.count()) * 1000u;
    state.buffer.clear();
    state.data_count = 0;
  }
};

eeprom_24xx::eeprom_24xx(eeprom_24xx_options options)
    : impl_{std::make_unique<impl>(std::move(options))}
{}

eeprom_24xx::~eeprom_24xx() = default;

auto eeprom_24xx::address() const -> std::uint8_t
{
  return impl_->options.address;
}

auto eeprom_24xx::start(bool read, vtime at) -> stm32::i2c_reply
{
  auto& self = *impl_;
  self.state.buffer.clear();
  self.state.data_count = 0;
  if(self.busy(at))
  {
    self.enter(phase::idle);
    return {.ack = false};
  }
  if(read)
  {
    self.enter(phase::reading);
    return {.ack = true};
  }
  self.enter(phase::word_address);
  self.state.address_count = 0;
  self.state.word_address  = 0;
  return {.ack = true};
}

auto eeprom_24xx::write(std::uint8_t byte, vtime) -> stm32::i2c_reply
{
  auto& self = *impl_;
  switch(self.step())
  {
    case phase::word_address:
      self.state.word_address = (self.state.word_address << 8) | byte;
      if(++self.state.address_count == self.options.address_bytes)
      {
        self.state.pointer = self.state.word_address % self.options.size;
        self.open_page();
        self.enter(phase::writing);
      }
      return {.ack = true};
    case phase::writing:
      self.state.buffer[self.in_page(self.state.data_count)] = std::byte{byte};
      ++self.state.data_count;
      return {.ack = true};
    case phase::idle:
    case phase::reading:
      return {.ack = false};
  }
  return {.ack = false};
}

auto eeprom_24xx::read(vtime) -> stm32::i2c_read_reply
{
  auto& self = *impl_;
  if(self.step() != phase::reading)
  {
    return {.byte = 0xFF};
  }
  auto const value   = std::to_integer<std::uint8_t>(self.array[self.state.pointer]);
  self.state.pointer = (self.state.pointer + 1) % self.options.size;
  return {.byte = value};
}

void eeprom_24xx::stop(vtime at)
{
  auto& self = *impl_;
  if(self.step() == phase::writing and self.state.data_count != 0)
  {
    self.commit(at);
  }
  self.state.buffer.clear();
  self.state.data_count = 0;
  self.enter(phase::idle);
}

void eeprom_24xx::reset(reset_kind kind)
{
  if(kind != reset_kind::power_on and kind != reset_kind::brown_out)
  {
    return;
  }
  impl_->state = {};
}

void eeprom_24xx::save(std::vector<std::byte>& out) const
{
  auto copy         = impl_->state;
  copy.size         = impl_->options.size;
  copy.image_digest = impl_->digest;
  auto const pages  = impl_->options.size / impl_->options.page_size;
  for(std::uint32_t page = 0; page < pages; ++page)
  {
    if(impl_->page_differs(page))
    {
      auto const first =
          impl_->array.begin() + std::ptrdiff_t{page} * std::ptrdiff_t{impl_->options.page_size};
      copy.pages.push_back({
          .index = page, .bytes = {first, first + impl_->options.page_size}
      });
    }
  }
  reflex::serde::bson::serializer{out}.dump(copy);
}

void eeprom_24xx::load(std::span<std::byte const> in)
{
  auto& self  = *impl_;
  auto  state = reflex::serde::bson::deserializer{in}.template load<eeprom_state>();
  if(state.size != self.options.size or state.image_digest != self.digest)
  {
    throw config_error(
        "eeprom_24xx", 0, "image", "a snapshot of an EEPROM of the same size and initial image");
  }
  self.array = self.initial;
  for(auto const& page : state.pages)
  {
    auto const at    = std::size_t{page.index} * self.options.page_size;
    auto const fits  = at + self.options.page_size <= self.array.size();
    auto const whole = page.bytes.size() == self.options.page_size;
    if(fits and whole)
    {
      std::ranges::copy(page.bytes, self.array.begin() + static_cast<std::ptrdiff_t>(at));
    }
  }
  state.pages.clear();
  self.state = std::move(state);
}

auto eeprom_24xx::contents() const -> std::span<std::byte const>
{
  return impl_->array;
}
}

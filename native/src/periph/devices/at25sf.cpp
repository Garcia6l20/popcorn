#include "image_file.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/devices/at25sf.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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
constexpr std::uint32_t page_size        = 256;
constexpr std::uint8_t  busy_bit         = 0x01;
constexpr std::uint8_t  wel_bit          = 0x02;
constexpr std::uint8_t  status1_writable = 0xFC;
constexpr std::uint8_t  status2_writable = 0x43;

enum opcode : std::uint8_t
{
  write_status    = 0x01,
  page_program    = 0x02,
  read            = 0x03,
  write_disable   = 0x04,
  read_status1    = 0x05,
  write_enable    = 0x06,
  fast_read       = 0x0B,
  erase_4k        = 0x20,
  write_status2   = 0x31,
  read_status2    = 0x35,
  volatile_enable = 0x50,
  erase_32k       = 0x52,
  chip_erase      = 0x60,
  reset_enable    = 0x66,
  read_ids        = 0x90,
  reset           = 0x99,
  read_jedec      = 0x9F,
  release         = 0xAB,
  power_down      = 0xB9,
  chip_erase_alt  = 0xC7,
  erase_64k       = 0xD8,
};

constexpr std::array<std::uint8_t, 21> known{
    write_status, page_program,   read,          write_disable, read_status1,    write_enable,
    fast_read,    erase_4k,       write_status2, read_status2,  volatile_enable, erase_32k,
    chip_erase,   reset_enable,   read_ids,      reset,         read_jedec,      release,
    power_down,   chip_erase_alt, erase_64k};

struct saved_page
{
  std::uint32_t          index{};
  std::vector<std::byte> bytes;

  friend auto operator==(saved_page const&, saved_page const&) -> bool = default;
};

struct at25sf_state
{
  std::uint32_t           size{};
  std::uint64_t           image_digest{};
  std::uint8_t            status1{};
  std::uint8_t            status2{};
  std::uint8_t            stored1{};
  std::uint8_t            stored2{};
  std::uint64_t           busy_until{};
  std::uint64_t           ready_from{};
  std::uint64_t           asleep_from{};
  std::uint8_t            asleep{};
  std::uint8_t            volatile_write{};
  std::uint8_t            reset_enabled{};
  std::uint8_t            selected{};
  std::uint8_t            active{};
  std::uint8_t            opcode{};
  std::uint32_t           count{};
  std::uint32_t           address{};
  std::uint8_t            status_in1{};
  std::uint8_t            status_in2{};
  std::vector<std::byte>  buffer;
  std::vector<saved_page> pages;

  friend auto operator==(at25sf_state const&, at25sf_state const&) -> bool = default;
};

auto ps_of(std::chrono::nanoseconds span) -> std::uint64_t
{
  return static_cast<std::uint64_t>(span.count()) * 1000u;
}

auto read_array(at25sf_options const& options) -> std::vector<std::byte>
{
  constexpr std::uint32_t block = 64u << 10;
  if(options.part.size == 0 or options.part.size % block != 0)
  {
    throw config_error(
        "at25sf", 0, "part.size",
        std::format("a non-zero multiple of 64 KB, not {} bytes", options.part.size));
  }
  if(options.initial.empty())
  {
    return read_image(options.image, options.part.size, "AT25SF");
  }
  if(not options.image.empty())
  {
    throw config_error("at25sf", 0, "initial", "initial bytes or an image file, not both");
  }
  if(options.initial.size() > options.part.size)
  {
    throw config_error(
        "at25sf", 0, "initial",
        std::format("at most {} initial bytes, not {}", options.part.size, options.initial.size()));
  }
  auto array = read_image({}, options.part.size, "AT25SF");
  std::ranges::copy(options.initial, array.begin());
  return array;
}
}

struct at25sf::impl
{
  at25sf_options         options;
  std::vector<std::byte> initial;
  std::vector<std::byte> array;
  std::uint64_t          digest;
  at25sf_state           state{};

  explicit impl(at25sf_options opts)
      : options{std::move(opts)}, initial{read_array(options)}, array{initial},
        digest{digest_of(initial)}
  {
    state.buffer.assign(page_size, std::byte{0xFF});
  }

  [[nodiscard]] auto busy(std::uint64_t at) const -> bool
  {
    return at < state.busy_until;
  }

  [[nodiscard]] auto asleep(std::uint64_t at) const -> bool
  {
    return state.asleep != 0 and at >= state.asleep_from;
  }

  [[nodiscard]] auto status1(std::uint64_t at) const -> std::uint8_t
  {
    return static_cast<std::uint8_t>((state.status1 & ~busy_bit) | (busy(at) ? busy_bit : 0));
  }

  [[nodiscard]] auto accepts(std::uint8_t code, std::uint64_t at) const -> bool
  {
    if(not std::ranges::contains(known, code) or at < state.ready_from)
    {
      return false;
    }
    if(asleep(at))
    {
      return code == release;
    }
    if(code == opcode::reset)
    {
      return state.reset_enabled != 0;
    }
    if(busy(at))
    {
      return code == read_status1 or code == read_status2 or code == reset_enable;
    }
    return true;
  }

  [[nodiscard]] auto read_next() -> std::uint8_t
  {
    auto const value = std::to_integer<std::uint8_t>(array[state.address % array.size()]);
    state.address    = (state.address + 1) % static_cast<std::uint32_t>(array.size());
    return value;
  }

  auto byte(std::uint8_t in, std::uint64_t at) -> std::uint8_t
  {
    auto const index = state.count++;
    if(index == 0)
    {
      state.opcode = in;
      state.active = accepts(in, at) ? 1 : 0;
      return 0xFF;
    }
    if(state.active == 0)
    {
      return 0xFF;
    }
    auto const addressed = index <= 3;
    if(addressed)
    {
      state.address = (state.address << 8 | in) & 0xFF'FFFFu;
    }
    switch(state.opcode)
    {
      case read_status1:
        return status1(at);
      case read_status2:
        return state.status2;
      case read_jedec:
      {
        auto const& part = options.part;
        return index == 1 ? part.manufacturer
             : index == 2 ? part.memory_type
             : index == 3 ? part.capacity
                          : 0xFF;
      }
      case read_ids:
      {
        if(addressed)
        {
          return 0xFF;
        }
        auto const second = ((index - 4) + (state.address & 1u)) % 2 != 0;
        return second ? options.part.device_id : options.part.manufacturer;
      }
      case release:
        return addressed ? 0xFF : options.part.device_id;
      case read:
        return addressed ? 0xFF : read_next();
      case fast_read:
        return index <= 4 ? 0xFF : read_next();
      case page_program:
        if(not addressed)
        {
          state.buffer[((state.address & 0xFFu) + index - 4) % page_size] = std::byte{in};
        }
        return 0xFF;
      case write_status:
        if(index == 1)
        {
          state.status_in1 = in;
        }
        else if(index == 2)
        {
          state.status_in2 = in;
        }
        return 0xFF;
      case write_status2:
        if(index == 1)
        {
          state.status_in2 = in;
        }
        return 0xFF;
      default:
        return 0xFF;
    }
  }

  [[nodiscard]] auto write_enabled() const -> bool
  {
    return (state.status1 & wel_bit) != 0;
  }

  void finish_write(std::uint64_t at, std::chrono::nanoseconds span)
  {
    state.status1 &= static_cast<std::uint8_t>(~wel_bit);
    state.busy_until = at + ps_of(span);
  }

  void erase(std::uint64_t at, std::uint32_t block, std::chrono::nanoseconds span)
  {
    auto const first = (state.address & ~(block - 1)) % static_cast<std::uint32_t>(array.size());
    std::fill_n(
        array.begin() + first, std::min<std::size_t>(block, array.size() - first), std::byte{0xFF});
    finish_write(at, span);
  }

  void execute(std::uint64_t at)
  {
    auto const code     = state.opcode;
    auto const count    = state.count;
    state.reset_enabled = code == reset_enable and count == 1 ? 1 : 0;
    switch(code)
    {
      case write_enable:
        state.status1 |= wel_bit;
        break;
      case write_disable:
        state.status1 &= static_cast<std::uint8_t>(~wel_bit);
        state.volatile_write = 0;
        break;
      case volatile_enable:
        state.volatile_write = 1;
        break;
      case opcode::reset:
        state.status1        = state.stored1;
        state.status2        = state.stored2;
        state.volatile_write = 0;
        state.busy_until     = std::min(state.busy_until, at);
        state.ready_from     = at + ps_of(options.reset);
        break;
      case page_program:
        if(write_enabled() and count > 4)
        {
          auto const base =
              (state.address & ~(page_size - 1)) % static_cast<std::uint32_t>(array.size());
          for(std::uint32_t offset = 0; offset < page_size; ++offset)
          {
            array[base + offset] &= state.buffer[offset];
          }
          finish_write(at, options.page_program);
        }
        break;
      case erase_4k:
      case erase_32k:
      case erase_64k:
        if(write_enabled() and count == 4)
        {
          auto const block = code == erase_4k  ? 4u << 10
                           : code == erase_32k ? 32u << 10
                                               : 64u << 10;
          auto const span  = code == erase_4k  ? options.erase_4k
                           : code == erase_32k ? options.erase_32k
                                               : options.erase_64k;
          erase(at, block, span);
        }
        break;
      case chip_erase:
      case chip_erase_alt:
        if(write_enabled() and count == 1)
        {
          std::ranges::fill(array, std::byte{0xFF});
          finish_write(at, options.chip_erase);
        }
        break;
      case write_status:
      case write_status2:
        if((write_enabled() or state.volatile_write != 0) and count >= 2 and count <= 3)
        {
          auto const second = code == write_status2 or count == 3;
          auto const stored = state.volatile_write == 0;
          auto const merge  = [](std::uint8_t old, std::uint8_t in, std::uint8_t mask) {
            return static_cast<std::uint8_t>((old & ~mask) | (in & mask));
          };
          if(code == write_status)
          {
            state.status1 = merge(state.status1, state.status_in1, status1_writable);
            if(stored)
            {
              state.stored1 = merge(state.stored1, state.status_in1, status1_writable);
            }
          }
          if(second)
          {
            state.status2 = merge(state.status2, state.status_in2, status2_writable);
            if(stored)
            {
              state.stored2 = merge(state.stored2, state.status_in2, status2_writable);
            }
          }
          if(stored)
          {
            finish_write(at, options.write_status);
          }
          state.volatile_write = 0;
        }
        break;
      case power_down:
        if(count == 1)
        {
          state.asleep      = 1;
          state.asleep_from = at + ps_of(options.power_down);
        }
        break;
      case release:
        if(state.asleep != 0)
        {
          state.asleep     = 0;
          state.ready_from = at + ps_of(options.release);
        }
        break;
      default:
        break;
    }
  }

  [[nodiscard]] auto page_differs(std::uint32_t page) const -> bool
  {
    auto const at = std::size_t{page} * page_size;
    return std::memcmp(array.data() + at, initial.data() + at, page_size) != 0;
  }
};

at25sf::at25sf(at25sf_options options) : impl_{std::make_unique<impl>(std::move(options))}
{}

at25sf::~at25sf() = default;

void at25sf::select(bool selected, vtime at)
{
  auto& state = impl_->state;
  if(selected)
  {
    state.selected = 1;
    state.active   = 0;
    state.count    = 0;
    state.address  = 0;
    state.buffer.assign(page_size, std::byte{0xFF});
    return;
  }
  if(state.selected != 0 and state.count != 0 and state.active != 0)
  {
    impl_->execute(at.ps);
  }
  else if(state.selected != 0 and state.count != 0)
  {
    state.reset_enabled = 0;
  }
  state.selected = 0;
  state.active   = 0;
}

auto at25sf::exchange(std::uint32_t out, std::uint8_t bits, vtime at) -> std::uint32_t
{
  auto&      state    = impl_->state;
  auto const all_ones = bits >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << bits) - 1u;
  if(state.selected == 0)
  {
    return all_ones;
  }
  if(bits % 8 != 0)
  {
    state.active = 0;
    state.count  = std::max<std::uint32_t>(state.count, 1);
    return all_ones;
  }
  auto in = std::uint32_t{};
  for(auto shift = static_cast<int>(bits) - 8; shift >= 0; shift -= 8)
  {
    auto const byte = static_cast<std::uint8_t>(out >> shift);
    in |= std::uint32_t{impl_->byte(byte, at.ps)} << shift;
  }
  return in;
}

void at25sf::reset(reset_kind kind)
{
  if(kind != reset_kind::power_on and kind != reset_kind::brown_out)
  {
    return;
  }
  auto&      state   = impl_->state;
  auto const stored1 = state.stored1;
  auto const stored2 = state.stored2;
  auto const size    = state.size;
  auto const digest  = state.image_digest;
  state              = {};
  state.stored1      = stored1;
  state.stored2      = stored2;
  state.status1      = static_cast<std::uint8_t>(stored1 & ~wel_bit);
  state.status2      = stored2;
  state.size         = size;
  state.image_digest = digest;
  state.buffer.assign(page_size, std::byte{0xFF});
}

void at25sf::save(std::vector<std::byte>& out) const
{
  auto copy         = impl_->state;
  copy.size         = static_cast<std::uint32_t>(impl_->array.size());
  copy.image_digest = impl_->digest;
  auto const pages  = static_cast<std::uint32_t>(impl_->array.size() / page_size);
  for(std::uint32_t page = 0; page < pages; ++page)
  {
    if(impl_->page_differs(page))
    {
      auto const first = impl_->array.begin() + std::ptrdiff_t{page} * page_size;
      copy.pages.push_back({
          .index = page, .bytes = {first, first + page_size}
      });
    }
  }
  reflex::serde::bson::serializer{out}.dump(copy);
}

void at25sf::load(std::span<std::byte const> in)
{
  auto state = reflex::serde::bson::deserializer{in}.template load<at25sf_state>();
  if(state.size != impl_->array.size() or state.image_digest != impl_->digest)
  {
    throw config_error(
        "at25sf", 0, "image", "a snapshot of an AT25SF with the same part size and initial image");
  }
  auto& array = impl_->array;
  array       = impl_->initial;
  for(auto const& page : state.pages)
  {
    auto const at = std::size_t{page.index} * page_size;
    if(at + page_size <= array.size() and page.bytes.size() == page_size)
    {
      std::ranges::copy(page.bytes, array.begin() + static_cast<std::ptrdiff_t>(at));
    }
  }
  state.pages.clear();
  if(state.buffer.size() != page_size)
  {
    state.buffer.assign(page_size, std::byte{0xFF});
  }
  impl_->state = std::move(state);
}

auto at25sf::contents() const -> std::span<std::byte const>
{
  return impl_->array;
}
}

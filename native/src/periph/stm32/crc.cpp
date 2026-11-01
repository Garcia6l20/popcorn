#include "factories.hpp"
#include "model_base.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
/**
 * @brief @p value with its low @p bits bit-reversed, the bits above dropped.
 * @param value Word to reverse.
 * @param bits Number of low bits reversed, 1 to 32.
 */
constexpr auto reversed(std::uint32_t value, unsigned bits) noexcept -> std::uint32_t
{
  std::uint32_t out = 0;
  for(unsigned bit = 0; bit < bits; ++bit)
  {
    out = (out << 1) | ((value >> bit) & 1u);
  }
  return out;
}

/**
 * @brief @p data with each @p group-bit group of its low @p width bits reversed in place.
 * @param data Right-aligned data.
 * @param width Data width in bits, a multiple of @p group.
 * @param group Group width in bits, 0 for no reversal.
 */
constexpr auto reversed_groups(std::uint32_t data, unsigned width, unsigned group) noexcept
    -> std::uint32_t
{
  if(group == 0)
  {
    return data;
  }
  std::uint32_t out = 0;
  for(unsigned at = 0; at < width; at += group)
  {
    out |= reversed(data >> at, group) << at;
  }
  return out;
}

static_assert(reversed_groups(0x1A2B'3C4D, 32, 8) == 0x58D4'3CB2);
static_assert(reversed_groups(0x1A2B'3C4D, 32, 16) == 0xD458'B23C);
static_assert(reversed_groups(0x1A2B'3C4D, 32, 32) == 0xB23C'D458);
static_assert(reversed(0x1122'3344, 32) == 0x22CC'4488);

constexpr std::array<unsigned, 4> polynomial_widths{32, 16, 8, 7};
constexpr std::array<unsigned, 4> input_groups{0, 8, 16, 32};
constexpr std::uint32_t           fixed_polynomial = 0x04C1'1DB7;
constexpr std::uint32_t           fixed_initial    = 0xFFFF'FFFF;

struct crc_state
{
  friend auto operator==(crc_state const&, crc_state const&) -> bool = default;
};

/**
 * @brief CRC calculation unit of the STM32U5 and STM32H5, RM0456 section 24, and of the
 *        STM32L1. A right-aligned 8-, 16- or 32-bit CRC_DR write feeds its bits MSB first through
 *        the low POLYSIZE bits of CRC_POL, after the REV_IN reversal by byte, half-word or word, a
 *        group no wider than the write. The running CRC is kept unreversed in the CRC_DR storage
 *        word, REV_OUT reverses the low POLYSIZE bits of the CRC_DR read only, the bits above read
 *        0. RM0456 gives both readings for 32-bit data only, unverified on silicon below that. The
 *        computation completes on the write: the input buffer hides the 4 HCLK latency, 24.3.3, so
 *        no access stalls. CR.RESET reloads CRC_DR from the low POLYSIZE bits of CRC_INIT and
 *        reads 0. A CRC_INIT write reloads CRC_DR, a CRC_POL write does not. CRC_IDR is storage,
 *        untouched by both. Every reset kind restores the description reset values. The RCC clock
 *        gate is not modelled. An even polynomial, which RM0456 does not support, logs once,
 *        raises model_error under strict_peripherals, and is computed anyway otherwise. A CRC_DR
 *        write that is not right-aligned and a write narrower than 32 bits to another register,
 *        which RM0456 24.4 forbids, log once, raise model_error under strict_peripherals, and are
 *        ignored. A narrow read of another register logs once, raises model_error under
 *        strict_peripherals, and returns the stored bits.
 *        The STM32L1 unit, RM0038 section 4, has DR, IDR and CR.RESET only: it computes CRC-32
 *        over the fixed polynomial 0x04C11DB7, RM0038 4.2, and CR.RESET loads 0xFFFFFFFF, 4.3,
 *        with no reversal. Its accesses are words, RM0038 4.4, so a narrow read of DR or CR logs
 *        once, raises model_error under strict_peripherals, and returns the stored lanes. A byte
 *        or half-word DR write does the same and feeds the word its byte lanes repeat: a byte 0xB0
 *        feeds 0xB0B0B0B0. For the DMA RM0038 11.3.4 states the repeated lanes, "the data are
 *        duplicated on the unused lanes of the HWDATA[31:0] bus". Inferred: a CPU byte or
 *        half-word store repeats its lanes the same way, and the unit ignores the access size,
 *        RM0038 4.3 "CRC computation is done on the whole 32-bit data word". IDR takes byte and
 *        half-word accesses, as ST's CMSIS header declares it a byte and LL_CRC_Write_IDR stores
 *        one. Deviation: a DR write completes with no stall, where RM0038 4.3 stalls it until the
 *        end of the 4 HCLK computation of 4.2.
 */
class crc final : public dispatch_model<crc, crc_state>, public bus_reset_listener
{
public:
  crc(model_context& ctx, peripheral_desc const& desc, crc_variant const& variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void control_written(std::uint32_t old, std::uint32_t now);
  void init_written(std::uint32_t old, std::uint32_t now);
  void stored(std::uint32_t, std::uint32_t)
  {}

private:
  void               restore_reset_values();
  [[nodiscard]] auto polynomial_width() const -> unsigned;
  [[nodiscard]] auto crc_mask() const -> std::uint32_t;
  [[nodiscard]] auto output() const -> std::uint32_t;
  void               feed(std::uint32_t data, unsigned width);
  void               set_crc(std::uint32_t value);
  void unmodelled(bool& logged, std::string_view name, std::uint32_t offset, std::string message);
  [[nodiscard]] auto name_at(std::uint32_t offset) const -> std::string_view;
  [[nodiscard]] auto polynomial() const -> std::uint32_t;
  [[nodiscard]] auto initial() const -> std::uint32_t;
  [[nodiscard]] auto served_narrow(std::string_view name) const -> bool;

  crc_variant    variant_;
  register_file& regs_;
  reg_ref        data_;
  reg_ref        init_;
  reg_ref        poly_;
  field_ref      reset_;
  field_ref      polysize_;
  field_ref      rev_in_;
  field_ref      rev_out_;
  bool           misaligned_logged_{};
  bool           even_logged_{};
  bool           narrow_write_logged_{};
  bool           narrow_read_logged_{};
};

constexpr std::array<write_entry<crc>, 3> crc_writes{
    {
     {"DR", &crc::stored},
     {"IDR", &crc::stored},
     {"CR", &crc::control_written},
     }
};

constexpr std::array<write_entry<crc>, 2> crc_programmable_writes{
    {
     {"INIT", &crc::init_written},
     {"POL", &crc::stored},
     }
};

crc::crc(model_context& ctx, peripheral_desc const& desc, crc_variant const& variant)
    : dispatch_model{ctx, desc, "CRC", crc_writes}, variant_{variant}, regs_{ctx.regs},
      data_{reg("DR")}, reset_{field("CR", "RESET")}
{
  expect_register("INIT", variant.programmable);
  expect_register("POL", variant.programmable);
  for(auto const name : {"POLYSIZE", "REV_IN", "REV_OUT"})
  {
    expect_field("CR", name, variant.programmable);
  }
  if(variant.programmable)
  {
    bind(crc_programmable_writes);
    init_     = reg("INIT");
    poly_     = reg("POL");
    polysize_ = field("CR", "POLYSIZE");
    rev_in_   = field("CR", "REV_IN");
    rev_out_  = field("CR", "REV_OUT");
  }
}

auto crc::polynomial() const -> std::uint32_t
{
  return variant_.programmable ? regs_.peek(poly_.offset) : fixed_polynomial;
}

auto crc::initial() const -> std::uint32_t
{
  return variant_.programmable ? regs_.peek(init_.offset) : fixed_initial;
}

auto crc::served_narrow(std::string_view name) const -> bool
{
  return variant_.byte_idr and name == "IDR";
}

auto crc::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) == data_.offset)
  {
    if(variant_.word_input and size != 4)
    {
      unmodelled(
          misaligned_logged_, "DR", offset,
          std::format(
              "{}-bit CRC_DR write at offset {:#x} fed as the word its byte lanes repeat, only "
              "32-bit accesses are allowed, {}",
              8 * size, offset, variant_.access_rule));
      auto const lanes = size == 1 ? (value & 0xFFu) * 0x0101'0101u : (value & 0xFFFFu) * 0x1'0001u;
      feed(lanes, 32);
      return {};
    }
    if(offset % 4 != 0)
    {
      unmodelled(
          misaligned_logged_, "DR", offset,
          std::format(
              "{}-bit CRC_DR write at offset {:#x} ignored, not right-aligned, {}", 8 * size,
              offset, variant_.access_rule));
      return {};
    }
    auto const width = 8u * size;
    if(variant_.programmable and (regs_.peek(poly_.offset) & 1u) == 0)
    {
      unmodelled(
          even_logged_, "POL", poly_.offset,
          "even polynomial, which RM0456 24.3.3 does not support, computed anyway");
    }
    feed(width >= 32 ? value : value & ((1u << width) - 1u), width);
    return {};
  }
  if(auto const name = name_at(offset); size != 4 and not name.empty() and not served_narrow(name))
  {
    unmodelled(
        narrow_write_logged_, name, offset,
        std::format(
            "{}-bit write to CRC_{} ignored, only 32-bit accesses are allowed, {}", 8 * size, name,
            variant_.access_rule));
    return {};
  }
  return dispatch_model::on_write(regs, offset, size, old, value, access);
}

auto crc::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const name = name_at(offset);
  if(size != 4
     and not name.empty()
     and not served_narrow(name)
     and (variant_.word_input or (offset & ~3u) != data_.offset))
  {
    unmodelled(
        narrow_read_logged_, name, offset,
        std::format(
            "{}-bit read of CRC_{}, only 32-bit accesses are allowed, {}", 8 * size, name,
            variant_.access_rule));
  }
  if((offset & ~3u) == data_.offset)
  {
    return read_lanes(output(), offset, size);
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

void crc::control_written(std::uint32_t, std::uint32_t now)
{
  if(reset_.of(now) == 0)
  {
    return;
  }
  regs_.poke(reset_.offset, reset_.with(now, 0));
  set_crc(initial());
}

void crc::init_written(std::uint32_t, std::uint32_t now)
{
  set_crc(now);
}

void crc::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void crc::on_bus_reset()
{
  restore_reset_values();
}

void crc::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
}

auto crc::polynomial_width() const -> unsigned
{
  return variant_.programmable ? polynomial_widths[get(regs_, polysize_)] : 32;
}

auto crc::crc_mask() const -> std::uint32_t
{
  auto const width = polynomial_width();
  return width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << width) - 1u;
}

auto crc::output() const -> std::uint32_t
{
  auto const value = regs_.peek(data_.offset);
  if(not variant_.programmable or get(regs_, rev_out_) == 0)
  {
    return value;
  }
  return reversed(value & crc_mask(), polynomial_width());
}

void crc::feed(std::uint32_t data, unsigned width)
{
  auto const top  = polynomial_width() - 1;
  auto const mask = crc_mask();
  auto const poly = polynomial() & mask;
  auto const group =
      variant_.programmable ? std::min(input_groups[get(regs_, rev_in_)], width) : 0u;
  auto const input = reversed_groups(data, width, group);
  auto       value = regs_.peek(data_.offset);
  for(auto bit = width; bit-- > 0;)
  {
    auto const feedback = ((value >> top) ^ (input >> bit)) & 1u;
    value               = (value << 1) & mask;
    if(feedback != 0)
    {
      value ^= poly;
    }
  }
  set_crc(value);
}

void crc::set_crc(std::uint32_t value)
{
  regs_.poke(data_.offset, value & crc_mask());
}

void crc::unmodelled(bool& logged, std::string_view name, std::uint32_t offset, std::string message)
{
  if(not std::exchange(logged, true))
  {
    warn(name, std::move(message));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, offset);
  }
}

auto crc::name_at(std::uint32_t offset) const -> std::string_view
{
  auto const found = std::ranges::find(desc_.registers, offset & ~3u, &register_desc::offset);
  return found != desc_.registers.end() ? std::string_view{found->name} : std::string_view{};
}
}

auto crc_factory(crc_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<crc>(ctx, desc, variant);
  };
}
}

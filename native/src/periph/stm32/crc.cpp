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
struct crc_state
{
  friend auto operator==(crc_state const&, crc_state const&) -> bool = default;
};

/**
 * @brief CRC calculation unit of the STM32U5, RM0456 section 24. A 32-bit CRC_DR write feeds
 *        the word MSB first through CRC_POL and CRC_DR reads the running CRC, kept in the
 *        CRC_DR storage word. The computation completes on the write: the input buffer hides
 *        the 4 HCLK latency, 24.3.3, so no access stalls. CR.RESET reloads CRC_DR from
 *        CRC_INIT and reads 0. A CRC_INIT write reloads CRC_DR, a CRC_POL write does not.
 *        CRC_IDR is storage, untouched by both. Every reset kind restores the description reset
 *        values. The RCC clock gate is not modelled.
 *        Not modelled, logged once, a model_error under strict_peripherals: 8- and 16-bit
 *        CRC_DR writes and a CRC_CR with POLYSIZE, REV_IN or REV_OUT other than 0, which feed
 *        nothing. A write narrower than 32 bits to another register, which RM0456 24.4
 *        forbids, logs once, raises model_error under strict_peripherals, and is ignored.
 */
class crc final : public dispatch_model<crc, crc_state>
{
public:
  crc(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
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
  void feed(std::uint32_t data, unsigned width);
  void set_crc(std::uint32_t value);
  void unmodelled(bool& logged, std::string_view name, std::uint32_t offset, std::string message);
  [[nodiscard]] auto name_at(std::uint32_t offset) const -> std::string_view;

  register_file& regs_;
  reg_ref        data_;
  reg_ref        init_;
  reg_ref        poly_;
  field_ref      reset_;
  field_ref      polysize_;
  field_ref      rev_in_;
  field_ref      rev_out_;
  bool           narrow_data_logged_{};
  bool           mode_logged_{};
  bool           narrow_other_logged_{};
};

constexpr std::array<write_entry<crc>, 5> crc_writes{
    {
     {"DR", &crc::stored},
     {"IDR", &crc::stored},
     {"CR", &crc::control_written},
     {"INIT", &crc::init_written},
     {"POL", &crc::stored},
     }
};

crc::crc(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "CRC", crc_writes}, regs_{ctx.regs}, data_{reg("DR")},
      init_{reg("INIT")}, poly_{reg("POL")}, reset_{field("CR", "RESET")},
      polysize_{field("CR", "POLYSIZE")}, rev_in_{field("CR", "REV_IN")},
      rev_out_{field("CR", "REV_OUT")}
{}

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
    if(size != 4)
    {
      unmodelled(
          narrow_data_logged_, "DR", offset, "8- and 16-bit CRC_DR writes not modelled, ignored");
      return {};
    }
    if(get(regs_, polysize_) != 0 or get(regs_, rev_in_) != 0 or get(regs_, rev_out_) != 0)
    {
      unmodelled(
          mode_logged_, "CR", offset,
          "POLYSIZE, REV_IN or REV_OUT other than 0 not modelled, the CRC_DR write is ignored");
      return {};
    }
    feed(value, 32);
    return {};
  }
  if(auto const name = name_at(offset); size != 4 and not name.empty())
  {
    unmodelled(
        narrow_other_logged_, name, offset,
        std::format(
            "{}-bit write to CRC_{} ignored, only 32-bit accesses are allowed, RM0456 24.4",
            8 * size, name));
    return {};
  }
  return dispatch_model::on_write(regs, offset, size, old, value, access);
}

void crc::control_written(std::uint32_t, std::uint32_t now)
{
  if(reset_.of(now) == 0)
  {
    return;
  }
  regs_.poke(reset_.offset, reset_.with(now, 0));
  set_crc(regs_.peek(init_.offset));
}

void crc::init_written(std::uint32_t, std::uint32_t now)
{
  set_crc(now);
}

void crc::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

void crc::feed(std::uint32_t data, unsigned width)
{
  auto const poly  = regs_.peek(poly_.offset);
  auto       value = regs_.peek(data_.offset);
  for(auto bit = width; bit-- > 0;)
  {
    auto const feedback = ((value >> 31) ^ (data >> bit)) & 1u;
    value <<= 1;
    if(feedback != 0)
    {
      value ^= poly;
    }
  }
  set_crc(value);
}

void crc::set_crc(std::uint32_t value)
{
  regs_.poke(data_.offset, value);
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

auto crc_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<crc>(ctx, desc);
  };
}
}

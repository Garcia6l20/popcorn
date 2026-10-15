#pragma once

#include <popcorn/core/clock.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/uart.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kit
{
inline constexpr std::uint32_t rng_base   = 0xC0DE'0000;
inline constexpr std::uint32_t rdr_empty  = 0xDEAD;
inline constexpr std::uint32_t frame_mask = 0x1FF;

class counter_model : public popcorn::periph::model
{
public:
  explicit counter_model(popcorn::peripheral_desc const& desc);

  [[nodiscard]] auto claims() const -> std::vector<std::string> override;
  void               reset(popcorn::periph::register_file& regs, popcorn::reset_kind kind) override;
  auto               on_read(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      popcorn::periph::access_ctx ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override;
  auto on_write(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      std::uint32_t                   old,
      std::uint32_t                   value,
      popcorn::periph::access_ctx     ctx) -> std::expected<void, popcorn::bus_error> override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

private:
  popcorn::register_desc const* data_;
  std::uint32_t                 reads_{};
};

class line_model : public popcorn::periph::model, public popcorn::periph::uart_port
{
public:
  line_model(popcorn::peripheral_desc const& desc, popcorn::core::clock const& clock);

  [[nodiscard]] auto claims() const -> std::vector<std::string> override;
  void               reset(popcorn::periph::register_file& regs, popcorn::reset_kind kind) override;
  auto               on_read(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      popcorn::periph::access_ctx ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override;
  auto on_write(
      popcorn::periph::register_file& regs,
      std::uint32_t                   offset,
      std::uint8_t                    size,
      std::uint32_t                   old,
      std::uint32_t                   value,
      popcorn::periph::access_ctx     ctx) -> std::expected<void, popcorn::bus_error> override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  void               receive_frames(std::span<popcorn::periph::host_frame const> frames) override;
  [[nodiscard]] auto queued() const -> std::size_t override;
  void               set_cts(bool ready, popcorn::vtime at) override;
  [[nodiscard]] auto pending_cts() const -> std::size_t override;
  [[nodiscard]] auto cts_ready() const -> bool override;
  [[nodiscard]] auto ready_to_receive() const -> bool override;
  void               attach(std::shared_ptr<popcorn::periph::uart_endpoint> far_end) override;

private:
  popcorn::register_desc const*                   transmit_;
  popcorn::register_desc const*                   receive_;
  popcorn::core::clock const*                     clock_;
  std::deque<std::uint16_t>                       received_;
  std::shared_ptr<popcorn::periph::uart_endpoint> far_;
  bool                                            cts_{true};
};

class target : public popcorn::periph::spi_target
{
public:
  void select(bool selected, popcorn::vtime at) override;
  auto exchange(std::uint32_t out, std::uint8_t bits, popcorn::vtime at) -> std::uint32_t override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;
};

class sub_target final : public target
{};
}

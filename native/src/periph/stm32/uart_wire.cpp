#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;

auto half_bits(std::uint32_t word, std::uint32_t half_stops) -> std::uint64_t
{
  return 2 * (1 + std::uint64_t{word}) + half_stops;
}

auto shifted(core::sys_time at, core::sys_time by) -> core::sys_time
{
  return {at.ps + by.ps};
}

auto earliest(std::optional<core::link_fault> kept, std::optional<core::link_fault> found)
    -> std::optional<core::link_fault>
{
  if(not kept or (found and found->at < kept->at))
  {
    return found;
  }
  return kept;
}
}

auto line_shape::frame_ps() const noexcept -> std::uint64_t
{
  return half_bits(word_bits, half_stops) * ps_per_second / (2 * std::uint64_t{baud});
}

auto line_shape::differs(line_frame const& frame) const noexcept -> bool
{
  if(frame.word_bits != word_bits or frame.half_stops != half_stops or frame.parity != parity)
  {
    return true;
  }
  auto const expected = frame_ps();
  auto const off =
      frame.frame_ps > expected ? frame.frame_ps - expected : expected - frame.frame_ps;
  return 100 * off > 3 * expected;
}

tap_queue::tap_queue(std::size_t capacity) : capacity_{capacity}
{}

void tap_queue::push(tap_frame const& frame)
{
  core::link_guard const held{mutex_};
  if(frames_.size() == capacity_)
  {
    frames_.pop_front();
    ++dropped_;
  }
  frames_.push_back(frame);
}

auto tap_queue::take() -> std::vector<tap_frame>
{
  core::link_guard const held{mutex_};
  std::vector<tap_frame> out(frames_.begin(), frames_.end());
  frames_.clear();
  return out;
}

auto tap_queue::dropped() const -> std::uint64_t
{
  core::link_guard const held{mutex_};
  return dropped_;
}

uart_wire::uart_wire(
    wire_ends                      ends,
    wire_lanes                     lanes,
    core::sys_time                 latency,
    line_shape                     declared,
    wire_receiver                  into,
    std::shared_ptr<uart_endpoint> forward,
    std::size_t                    tap_capacity)
    : ends_{ends}, lanes_{lanes}, latency_{latency}, declared_{declared}, into_{std::move(into)},
      forward_{std::move(forward)}, tap_{tap_capacity}
{}

auto uart_wire::mailbox(std::uint64_t window) -> std::vector<detail::wire_post>&
{
  return window % 2 == 0 ? state_.mail_even : state_.mail_odd;
}

void uart_wire::started(line_frame const& frame)
{
  core::time_map const map{state_.sender_base};
  mailbox(state_.window)
      .push_back(
          {.index = state_.started++,
           .frame = frame,
           .start = map.to_system(frame.start),
           .end   = map.to_system(frame.end)});
  if(forward_)
  {
    forward_->started(frame);
  }
}

void uart_wire::aborted(vtime at)
{
  auto const system = core::time_map{state_.sender_base}.to_system(at);
  mailbox(state_.window).push_back({.abort = true, .start = system, .end = system});
  if(forward_)
  {
    forward_->aborted(at);
  }
}

void uart_wire::transmitted(std::uint16_t data, vtime at, bool brk)
{
  if(forward_)
  {
    forward_->transmitted(data, at, brk);
  }
}

auto uart_wire::sender() const noexcept -> std::size_t
{
  return ends_.sender;
}

auto uart_wire::receiver() const noexcept -> std::size_t
{
  return ends_.receiver;
}

auto uart_wire::latency() const noexcept -> core::sys_time
{
  return latency_;
}

void uart_wire::enter_window(std::uint64_t window, core::time_map sender)
{
  state_.window      = window;
  state_.sender_base = sender.base();
}

auto uart_wire::check_shape(detail::wire_post const& post) -> std::optional<core::link_fault>
{
  if(state_.mismatch_logged or not declared_.differs(post.frame))
  {
    return std::nullopt;
  }
  state_.mismatch_logged = true;
  auto const baud        = half_bits(post.frame.word_bits, post.frame.half_stops)
                         * ps_per_second
                         / (2 * std::max<std::uint64_t>(post.frame.frame_ps, 1));
  if(into_.log)
  {
    into_.log(
        {.kind       = core::log_kind::model_warning,
         .peripheral = into_.unit,
         .message    = std::format(
             "frame at {} baud, {} data bits, {} half stop bits on a link declared {} baud, {} "
             "data bits, {} half stop bits",
             baud, post.frame.word_bits, post.frame.half_stops, declared_.baud, declared_.word_bits,
             declared_.half_stops)});
  }
  if(not into_.strict)
  {
    return std::nullopt;
  }
  return core::link_fault{
      .at     = post.start,
      .member = ends_.receiver,
      .error  = std::make_exception_ptr(model_error(into_.unit, "", 0))};
}

void uart_wire::collect(std::vector<detail::wire_post> const& posts, std::vector<tap_frame>& into)
    const
{
  for(auto const& post : posts)
  {
    if(post.abort)
    {
      auto const lost = std::ranges::find_if(into, [&](tap_frame const& frame) {
        return not frame.lost and frame.start < post.start and post.start < frame.end;
      });
      if(lost != into.end())
      {
        lost->lost    = true;
        lost->end     = post.start;
        lost->arrival = shifted(post.start, latency_);
      }
      continue;
    }
    into.push_back(
        {.from    = ends_.sender,
         .to      = ends_.receiver,
         .index   = post.index,
         .data    = post.frame.data,
         .start   = post.start,
         .end     = post.end,
         .arrival = shifted(post.end, latency_),
         .brk     = post.frame.kind == frame_kind::brk});
  }
}

void uart_wire::emit(std::vector<tap_frame>& from, core::sys_time through)
{
  auto const ready =
      std::ranges::find_if(from, [&](tap_frame const& frame) { return frame.end > through; });
  for(auto const& frame : std::span{from.begin(), ready})
  {
    if(frame.index >= state_.tapped)
    {
      tap_.push(frame);
      state_.tapped = frame.index + 1;
    }
  }
  from.erase(from.begin(), ready);
}

auto uart_wire::drain(std::uint64_t window, core::sys_time barrier, core::time_map receiver)
    -> std::optional<core::link_fault>
{
  auto&                           posts = mailbox(window);
  std::optional<core::link_fault> fault;
  for(auto const& post : posts)
  {
    if(post.abort)
    {
      into_.port->cancel_in_flight(receiver.to_local(shifted(post.start, latency_)), lanes_.peer);
      continue;
    }
    fault       = earliest(fault, check_shape(post));
    auto frame  = post.frame;
    frame.start = receiver.to_local(shifted(post.start, latency_));
    frame.end   = receiver.to_local(shifted(post.end, latency_));
    into_.port->receive_at(frame, lanes_.peer);
  }
  collect(posts, state_.untapped);
  posts.clear();
  emit(state_.untapped, barrier);
  return fault;
}

void uart_wire::finish(core::sys_time at)
{
  auto pending = state_.untapped;
  collect(mailbox(state_.window + 1), pending);
  collect(mailbox(state_.window), pending);
  emit(pending, at);
}

void uart_wire::save(std::vector<std::byte>& out) const
{
  reflex::serde::bson::serializer{out}.dump(state_);
}

void uart_wire::load(std::span<std::byte const> in)
{
  try
  {
    state_ = reflex::serde::bson::deserializer{in}.load<detail::wire_state>();
  }
  catch(std::runtime_error const& failure)
  {
    throw config_error(
        "uart wire state", 0, "bytes",
        std::format("a state uart_wire::save wrote: {}", failure.what()));
  }
}

auto uart_wire::tap() noexcept -> tap_queue&
{
  return tap_;
}
}

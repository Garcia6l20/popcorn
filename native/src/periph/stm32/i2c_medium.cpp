#include "i2c_medium.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/link_guard.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace popcorn::periph::stm32
{
namespace detail
{
constexpr std::uint8_t forth_stop    = 3;
constexpr std::uint8_t forth_release = 4;

struct i2c_forth_post
{
  std::uint8_t   kind{};
  std::uint64_t  unit{};
  core::sys_time stamp{};
  std::uint8_t   byte{};
  std::uint8_t   ack{};
  std::uint64_t  phase_ps{};

  friend auto operator==(i2c_forth_post const&, i2c_forth_post const&) -> bool = default;
};

struct i2c_back_post
{
  std::uint64_t  unit{};
  core::sys_time stamp{};
  core::sys_time sent{};
  std::uint8_t   phase{};
  std::uint8_t   byte{};
  std::uint8_t   ack{};
  std::uint8_t   dropped{};

  friend auto operator==(i2c_back_post const&, i2c_back_post const&) -> bool = default;
};

struct i2c_ticket
{
  std::uint64_t unit{};
  std::uint64_t ticket{};
  std::uint8_t  phase{};

  friend auto operator==(i2c_ticket const&, i2c_ticket const&) -> bool = default;
};

struct i2c_forth_state
{
  std::string                 kind{"i2c_forth"};
  std::vector<i2c_forth_post> mail_even;
  std::vector<i2c_forth_post> mail_odd;
  std::vector<i2c_forth_post> carried;
  std::uint64_t               window{};
  core::sys_time              sender_base{};
  std::uint64_t               unit{};
  std::uint8_t                open{};
  std::uint8_t                acknowledged{};
  std::uint64_t               address_unit{};
  std::vector<i2c_ticket>     tickets;
  std::vector<core::sys_time> resets;
  core::sys_time              last_barrier{};
  std::uint64_t               refused{};
  std::vector<i2c_back_post>  undriven;

  friend auto operator==(i2c_forth_state const&, i2c_forth_state const&) -> bool = default;
};

struct i2c_owed
{
  std::uint64_t  unit{};
  core::sys_time sent{};
  std::uint8_t   phase{};
  std::uint8_t   byte{};
  std::uint8_t   ack{};
  core::sys_time end{};

  friend auto operator==(i2c_owed const&, i2c_owed const&) -> bool = default;
};

struct i2c_back_state
{
  std::string                 kind{"i2c_back"};
  std::vector<i2c_back_post>  mail_even;
  std::vector<i2c_back_post>  mail_odd;
  std::vector<i2c_back_post>  carried;
  std::uint64_t               window{};
  core::sys_time              sender_base{};
  std::vector<i2c_owed>       owed;
  std::vector<core::sys_time> resets;
  core::sys_time              last_barrier{};

  friend auto operator==(i2c_back_state const&, i2c_back_state const&) -> bool = default;
};

namespace
{
auto plus(core::sys_time at, std::int64_t ps) -> core::sys_time
{
  return {at.ps + ps};
}

auto latest_after(std::vector<core::sys_time> const& resets, core::sys_time stamp)
    -> std::optional<core::sys_time>
{
  std::optional<core::sys_time> found;
  for(auto const& reset : resets)
  {
    if(reset > stamp and (not found or reset > *found))
    {
      found = reset;
    }
  }
  return found;
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

auto is_read(std::uint8_t phase) -> bool
{
  return static_cast<i2c_phase>(phase) == i2c_phase::read;
}

template <typename State> auto decoded(std::span<std::byte const> in) -> State
{
  State loaded;
  try
  {
    loaded = reflex::serde::bson::deserializer{in}.load<State>();
  }
  catch(std::runtime_error const& failure)
  {
    throw config_error(
        "i2c link state", 0, "bytes",
        std::format("a state an I2C link of kind {} saved: {}", State{}.kind, failure.what()));
  }
  if(loaded.kind != State{}.kind)
  {
    throw config_error(
        "i2c link state", 0, "kind",
        std::format("an I2C link state of kind {}, the bytes hold {}", State{}.kind, loaded.kind));
  }
  return loaded;
}
}

class i2c_forth;
class i2c_back;

class i2c_controller_proxy final : public i2c_link_target
{
public:
  explicit i2c_controller_proxy(i2c_forth& link) : link_{link}
  {}

  void linked(i2c_link_port& port, core::clock const& clock) override
  {
    port_  = &port;
    clock_ = &clock;
  }

  void attached(i2c_target_port& port) override
  {
    target_ = &port;
  }

  void detached() noexcept override
  {
    port_   = nullptr;
    clock_  = nullptr;
    target_ = nullptr;
  }

  [[nodiscard]] auto address() const -> std::uint8_t override
  {
    return 0;
  }

  [[nodiscard]] auto start(bool, vtime) -> i2c_reply override
  {
    return {.ack = false};
  }

  [[nodiscard]] auto write(std::uint8_t, vtime) -> i2c_reply override
  {
    return {.ack = false};
  }

  [[nodiscard]] auto read(vtime) -> i2c_read_reply override
  {
    return {};
  }

  [[nodiscard]] auto matches(std::uint8_t address, vtime at) -> bool override;

  [[nodiscard]] auto start_deferred(std::uint8_t address, bool read, vtime at, std::uint64_t ticket)
      -> std::optional<i2c_reply> override;

  [[nodiscard]] auto write_deferred(std::uint8_t byte, vtime at, std::uint64_t ticket)
      -> std::optional<i2c_reply> override;

  [[nodiscard]] auto read_deferred(vtime at, bool ack, std::uint64_t ticket)
      -> std::optional<i2c_read_reply> override;

  void stop(vtime at) override;

  void bus_released(vtime at) override;

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto port() const noexcept -> i2c_link_port*
  {
    return port_;
  }

  [[nodiscard]] auto target() const noexcept -> i2c_target_port*
  {
    return target_;
  }

  [[nodiscard]] auto clock() const noexcept -> core::clock const*
  {
    return clock_;
  }

  [[nodiscard]] auto offset() const -> std::uint64_t
  {
    return clock_->since_power_on().ps - clock_->time().ps;
  }

private:
  [[nodiscard]] auto live() const -> bool
  {
    return clock_ != nullptr and not port_->muted(*this);
  }

  [[nodiscard]] auto local(vtime at) const -> vtime
  {
    return vtime{at.ps - offset()};
  }

  i2c_forth&         link_;
  i2c_link_port*     port_{};
  core::clock const* clock_{};
  i2c_target_port*   target_{};
};

class i2c_target_proxy final : public i2c_link_controller
{
public:
  explicit i2c_target_proxy(i2c_back& link) : link_{link}
  {}

  void linked(i2c_controller_port& port, core::clock const&) override
  {
    port_ = &port;
  }

  void unlinked() noexcept override
  {
    port_ = nullptr;
  }

  void answered(std::uint64_t unit, i2c_phase phase, i2c_answer answer, vtime at) override;
  void dropped(std::uint64_t unit, i2c_phase phase, vtime at) override;

  [[nodiscard]] auto port() const noexcept -> i2c_controller_port*
  {
    return port_;
  }

private:
  i2c_back&            link_;
  i2c_controller_port* port_{};
};

struct i2c_pair
{
  i2c_pair(
      std::size_t                    target_index,
      std::size_t                    controller,
      i2c_target_end                 end,
      core::sys_time                 latency,
      std::shared_ptr<i2c_tap_queue> tap);

  std::size_t                           index;
  std::size_t                           controller;
  i2c_target_end                        end;
  core::sys_time                        latency;
  std::shared_ptr<i2c_tap_queue>        tap;
  std::shared_ptr<i2c_forth>            forth;
  std::shared_ptr<i2c_back>             back;
  std::shared_ptr<i2c_controller_proxy> controller_proxy;
  std::shared_ptr<i2c_target_proxy>     target_proxy;
  std::size_t                           owed_peak{};
  std::size_t                           tickets_peak{};
};

class i2c_forth final : public core::member_link
{
public:
  explicit i2c_forth(i2c_pair& pair) : pair_{pair}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return pair_.controller;
  }

  [[nodiscard]] auto receiver() const noexcept -> std::size_t override
  {
    return pair_.end.member;
  }

  [[nodiscard]] auto latency() const noexcept -> core::sys_time override
  {
    return pair_.latency;
  }

  void enter_window(std::uint64_t window, core::time_map sender) override
  {
    state_.window      = window;
    state_.sender_base = sender.base();
    auto& proxy        = *pair_.controller_proxy;
    if(proxy.port() != nullptr and proxy.port()->muted(proxy))
    {
      proxy.port()->unmute(proxy);
    }
  }

  void sender_reset(core::sys_time at) override
  {
    if(state_.open != 0)
    {
      mailbox().push_back({.kind = forth_stop, .unit = ++state_.unit, .stamp = at});
    }
    state_.open         = 0;
    state_.acknowledged = 0;
    state_.tickets.clear();
    auto& proxy = *pair_.controller_proxy;
    if(proxy.port() != nullptr)
    {
      proxy.port()->mute(proxy);
    }
  }

  void receiver_reset(core::sys_time at) override
  {
    state_.resets.push_back(at);
  }

  auto drain(
      std::uint64_t         window,
      core::sys_time const& barrier,
      core::sys_time const& next,
      core::time_map        receiver) -> std::optional<core::link_fault> override
  {
    auto const before = state_.last_barrier;
    std::erase_if(state_.resets, [&](core::sys_time const& reset) {
      return plus(reset, pair_.latency.ps) <= before;
    });
    auto  posts = std::exchange(state_.carried, {});
    auto& mail  = window % 2 == 0 ? state_.mail_even : state_.mail_odd;
    posts.insert(posts.end(), mail.begin(), mail.end());
    mail.clear();
    std::optional<core::link_fault> fault;
    for(auto const& post : posts)
    {
      if(not state_.carried.empty() or plus(post.stamp, pair_.latency.ps) >= next)
      {
        state_.carried.push_back(post);
        continue;
      }
      try
      {
        hand(post, receiver);
      }
      catch(...)
      {
        fault = earliest(
            fault, core::link_fault{
                       .at     = std::max(post.stamp, before),
                       .member = pair_.end.member,
                       .error  = std::current_exception()});
      }
    }
    state_.last_barrier = barrier;
    return fault;
  }

  [[nodiscard]] auto quiet() const -> bool override
  {
    return state_.mail_even.empty()
       and state_.mail_odd.empty()
       and state_.carried.empty()
       and state_.undriven.empty();
  }

  void finish(core::sys_time) override
  {}

  void save(std::vector<std::byte>& out) const override
  {
    reflex::serde::bson::serializer{out}.dump(state_);
  }

  void load(std::span<std::byte const> in) override
  {
    state_      = decoded<i2c_forth_state>(in);
    auto& proxy = *pair_.controller_proxy;
    if(proxy.port() != nullptr)
    {
      proxy.port()->unmute(proxy);
    }
  }

  void forget()
  {
    state_.acknowledged = 0;
  }

  [[nodiscard]] auto acknowledged() const noexcept -> bool
  {
    return state_.acknowledged != 0;
  }

  void post_phase(
      i2c_phase     phase,
      std::uint8_t  byte,
      bool          ack,
      vtime         local,
      std::uint64_t ticket,
      std::uint64_t phase_ps)
  {
    auto const unit = ++state_.unit;
    mailbox().push_back(
        {.kind     = static_cast<std::uint8_t>(phase),
         .unit     = unit,
         .stamp    = core::time_map{state_.sender_base}.to_system(local),
         .byte     = byte,
         .ack      = static_cast<std::uint8_t>(ack ? 1 : 0),
         .phase_ps = phase_ps});
    state_.tickets.push_back(
        {.unit = unit, .ticket = ticket, .phase = static_cast<std::uint8_t>(phase)});
    pair_.tickets_peak = std::max(pair_.tickets_peak, state_.tickets.size());
    if(phase == i2c_phase::address)
    {
      state_.open         = 1;
      state_.acknowledged = 0;
      state_.address_unit = unit;
    }
  }

  void post_stop(vtime local)
  {
    if(state_.open == 0)
    {
      return;
    }
    mailbox().push_back(
        {.kind  = forth_stop,
         .unit  = ++state_.unit,
         .stamp = core::time_map{state_.sender_base}.to_system(local)});
    state_.open         = 0;
    state_.acknowledged = 0;
  }

  void post_release(vtime local)
  {
    if(state_.open == 0)
    {
      return;
    }
    mailbox().push_back(
        {.kind  = forth_release,
         .unit  = state_.unit,
         .stamp = core::time_map{state_.sender_base}.to_system(local)});
  }

  void reply(i2c_back_post const& post, core::time_map receiver)
  {
    auto const found = std::ranges::find(state_.tickets, post.unit, &i2c_ticket::unit);
    auto&      proxy = *pair_.controller_proxy;
    if(found == state_.tickets.end() or proxy.target() == nullptr)
    {
      ++state_.refused;
      return;
    }
    auto const ticket = found->ticket;
    state_.tickets.erase(found);
    auto const local = receiver.to_local(plus(post.stamp, pair_.latency.ps));
    auto const at    = vtime{local.ps + proxy.offset()};
    if(is_read(post.phase))
    {
      proxy.target()->complete_read(ticket, {.byte = post.byte}, at, pair_.end.back_lane);
      return;
    }
    if(static_cast<i2c_phase>(post.phase) == i2c_phase::address
       and post.unit == state_.address_unit)
    {
      state_.acknowledged = post.ack;
    }
    proxy.target()->complete(ticket, {.ack = post.ack != 0}, at, pair_.end.back_lane);
  }

  void complete_open() noexcept
  {
    auto& proxy   = *pair_.controller_proxy;
    auto  tickets = std::exchange(state_.tickets, {});
    if(proxy.target() == nullptr)
    {
      return;
    }
    for(auto const& open : tickets)
    {
      try
      {
        auto const at = proxy.clock()->since_power_on();
        if(is_read(open.phase))
        {
          proxy.target()->complete_read(open.ticket, {}, at, pair_.end.back_lane);
        }
        else
        {
          proxy.target()->complete(open.ticket, {.ack = false}, at, pair_.end.back_lane);
        }
      }
      catch(...)
      {}
    }
  }

  auto take_undriven() -> std::vector<i2c_back_post>
  {
    return std::exchange(state_.undriven, {});
  }

  [[nodiscard]] auto state() const noexcept -> i2c_forth_state const&
  {
    return state_;
  }

private:
  auto mailbox() -> std::vector<i2c_forth_post>&
  {
    return state_.window % 2 == 0 ? state_.mail_even : state_.mail_odd;
  }

  void hand(i2c_forth_post const& post, core::time_map receiver);

  i2c_pair&       pair_;
  i2c_forth_state state_;
};

class i2c_back final : public core::member_link
{
public:
  explicit i2c_back(i2c_pair& pair) : pair_{pair}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return pair_.end.member;
  }

  [[nodiscard]] auto receiver() const noexcept -> std::size_t override
  {
    return pair_.controller;
  }

  [[nodiscard]] auto latency() const noexcept -> core::sys_time override
  {
    return pair_.latency;
  }

  void enter_window(std::uint64_t window, core::time_map sender) override
  {
    state_.window      = window;
    state_.sender_base = sender.base();
    for(auto const& post : pair_.forth->take_undriven())
    {
      mailbox().push_back(post);
    }
  }

  void sender_reset(core::sys_time) override
  {}

  void receiver_reset(core::sys_time at) override
  {
    state_.resets.push_back(at);
  }

  auto drain(
      std::uint64_t         window,
      core::sys_time const& barrier,
      core::sys_time const& next,
      core::time_map        receiver) -> std::optional<core::link_fault> override
  {
    auto const before = state_.last_barrier;
    std::erase_if(state_.resets, [&](core::sys_time const& reset) {
      return plus(reset, pair_.latency.ps) <= before;
    });
    auto  posts = std::exchange(state_.carried, {});
    auto& mail  = window % 2 == 0 ? state_.mail_even : state_.mail_odd;
    posts.insert(posts.end(), mail.begin(), mail.end());
    mail.clear();
    std::optional<core::link_fault> fault;
    for(auto const& post : posts)
    {
      if(latest_after(state_.resets, post.stamp))
      {
        show(post);
        continue;
      }
      if(not state_.carried.empty() or plus(post.stamp, pair_.latency.ps) >= next)
      {
        state_.carried.push_back(post);
        continue;
      }
      show(post);
      try
      {
        pair_.forth->reply(post, receiver);
      }
      catch(...)
      {
        fault = earliest(
            fault, core::link_fault{
                       .at     = std::max(post.stamp, before),
                       .member = pair_.controller,
                       .error  = std::current_exception()});
      }
    }
    state_.last_barrier = barrier;
    return fault;
  }

  [[nodiscard]] auto quiet() const -> bool override
  {
    return state_.mail_even.empty() and state_.mail_odd.empty() and state_.carried.empty();
  }

  void finish(core::sys_time) override
  {}

  void save(std::vector<std::byte>& out) const override
  {
    reflex::serde::bson::serializer{out}.dump(state_);
  }

  void load(std::span<std::byte const> in) override
  {
    state_ = decoded<i2c_back_state>(in);
  }

  void owe(i2c_owed owed)
  {
    state_.owed.push_back(owed);
    pair_.owed_peak = std::max(pair_.owed_peak, state_.owed.size());
  }

  void answer_phase(std::uint64_t unit, i2c_answer answer, vtime at)
  {
    auto const found = std::ranges::find(state_.owed, unit, &i2c_owed::unit);
    if(found == state_.owed.end())
    {
      return;
    }
    auto const owed = *found;
    state_.owed.erase(found);
    auto const read = is_read(owed.phase);
    mailbox().push_back(
        {.unit  = owed.unit,
         .stamp = core::time_map{state_.sender_base}.to_system(at),
         .sent  = owed.sent,
         .phase = owed.phase,
         .byte  = read ? answer.byte : owed.byte,
         .ack   = static_cast<std::uint8_t>(read ? owed.ack : (answer.ack ? 1 : 0))});
  }

  void answer_dropped(std::uint64_t unit, vtime at)
  {
    auto const found = std::ranges::find(state_.owed, unit, &i2c_owed::unit);
    if(found == state_.owed.end())
    {
      return;
    }
    auto post  = undriven(*found);
    post.stamp = core::time_map{state_.sender_base}.to_system(at);
    state_.owed.erase(found);
    mailbox().push_back(post);
  }

  [[nodiscard]] auto state() const noexcept -> i2c_back_state const&
  {
    return state_;
  }

  static auto undriven(i2c_owed const& owed) -> i2c_back_post
  {
    auto const read = is_read(owed.phase);
    return {
        .unit    = owed.unit,
        .stamp   = owed.end,
        .sent    = owed.sent,
        .phase   = owed.phase,
        .byte    = read ? std::uint8_t{0xFF} : owed.byte,
        .ack     = read ? owed.ack : std::uint8_t{0},
        .dropped = 1};
  }

  void give_back()
  {
    for(auto const& owed : std::exchange(state_.owed, {}))
    {
      mailbox().push_back(undriven(owed));
    }
  }

private:
  auto mailbox() -> std::vector<i2c_back_post>&
  {
    return state_.window % 2 == 0 ? state_.mail_even : state_.mail_odd;
  }

  void show(i2c_back_post const& post)
  {
    pair_.tap->push(
        {.target  = pair_.index,
         .unit    = post.unit,
         .phase   = static_cast<i2c_phase>(post.phase),
         .byte    = post.byte,
         .ack     = post.ack != 0,
         .at      = post.sent,
         .answer  = post.stamp,
         .dropped = post.dropped != 0});
  }

  i2c_pair&      pair_;
  i2c_back_state state_;
};

void i2c_forth::hand(i2c_forth_post const& post, core::time_map receiver)
{
  auto* const port  = pair_.target_proxy->port();
  auto const  reset = latest_after(state_.resets, post.stamp);
  auto const  start = plus(post.stamp, pair_.latency.ps);
  auto const  lane  = pair_.end.forth_lane;
  if(post.kind == forth_stop)
  {
    if(port != nullptr and not reset)
    {
      port->stop(receiver.to_local(start), lane, post.unit);
    }
    return;
  }
  if(post.kind == forth_release)
  {
    if(port != nullptr and not reset)
    {
      pair_.end.port->bus_released(receiver.to_local(start), post.unit);
    }
    return;
  }
  auto const end  = plus(start, static_cast<std::int64_t>(post.phase_ps));
  auto const owed = i2c_owed{
      .unit  = post.unit,
      .sent  = post.stamp,
      .phase = post.kind,
      .byte  = post.byte,
      .ack   = post.ack,
      .end   = reset ? std::max(*reset, end) : end};
  if(port == nullptr)
  {
    state_.undriven.push_back(i2c_back::undriven(owed));
    return;
  }
  auto const phase = static_cast<i2c_phase>(post.kind);
  if(reset)
  {
    port->drop(phase, receiver.to_local(owed.end), lane, post.unit);
    pair_.back->owe(owed);
    return;
  }
  auto const at = receiver.to_local(start);
  switch(phase)
  {
    case i2c_phase::address:
      port->start(
          static_cast<std::uint8_t>(post.byte >> 1), (post.byte & 1u) != 0, post.phase_ps, at, lane,
          post.unit);
      break;
    case i2c_phase::write:
      port->write(post.byte, post.phase_ps, at, lane, post.unit);
      break;
    case i2c_phase::read:
      port->read(post.ack != 0, post.phase_ps, at, lane, post.unit);
      break;
  }
  pair_.back->owe(owed);
}

auto i2c_controller_proxy::matches(std::uint8_t, vtime) -> bool
{
  link_.forget();
  return true;
}

auto i2c_controller_proxy::start_deferred(
    std::uint8_t  address,
    bool          read,
    vtime         at,
    std::uint64_t ticket) -> std::optional<i2c_reply>
{
  if(not live())
  {
    return i2c_reply{.ack = false};
  }
  link_.post_phase(
      i2c_phase::address, static_cast<std::uint8_t>(address << 1 | (read ? 1u : 0u)), false,
      local(at), ticket, port_->phase_time());
  return std::nullopt;
}

auto i2c_controller_proxy::write_deferred(std::uint8_t byte, vtime at, std::uint64_t ticket)
    -> std::optional<i2c_reply>
{
  if(not live() or not link_.acknowledged())
  {
    return i2c_reply{.ack = false};
  }
  link_.post_phase(i2c_phase::write, byte, false, local(at), ticket, port_->phase_time());
  return std::nullopt;
}

auto i2c_controller_proxy::read_deferred(vtime at, bool ack, std::uint64_t ticket)
    -> std::optional<i2c_read_reply>
{
  if(not live() or not link_.acknowledged())
  {
    return i2c_read_reply{};
  }
  link_.post_phase(i2c_phase::read, 0xFF, ack, local(at), ticket, port_->phase_time());
  return std::nullopt;
}

void i2c_controller_proxy::stop(vtime at)
{
  if(not live())
  {
    return;
  }
  link_.post_stop(local(at));
}

void i2c_controller_proxy::bus_released(vtime at)
{
  if(not live())
  {
    return;
  }
  link_.post_release(local(at));
}

void i2c_target_proxy::answered(std::uint64_t unit, i2c_phase, i2c_answer answer, vtime at)
{
  link_.answer_phase(unit, answer, at);
}

void i2c_target_proxy::dropped(std::uint64_t unit, i2c_phase, vtime at)
{
  link_.answer_dropped(unit, at);
}

i2c_pair::i2c_pair(
    std::size_t                    target_index,
    std::size_t                    controller_member,
    i2c_target_end                 target,
    core::sys_time                 link_latency,
    std::shared_ptr<i2c_tap_queue> phases)
    : index{target_index}, controller{controller_member}, end{target}, latency{link_latency},
      tap{std::move(phases)}
{}
}

i2c_tap_queue::i2c_tap_queue(std::size_t capacity) : capacity_{capacity}
{}

void i2c_tap_queue::push(i2c_tap_phase const& phase)
{
  core::link_guard const held{mutex_};
  if(phases_.size() == capacity_)
  {
    phases_.pop_front();
    ++dropped_;
  }
  phases_.push_back(phase);
}

auto i2c_tap_queue::take() -> std::vector<i2c_tap_phase>
{
  core::link_guard const     held{mutex_};
  std::vector<i2c_tap_phase> out{phases_.begin(), phases_.end()};
  phases_.clear();
  return out;
}

auto i2c_tap_queue::dropped() const -> std::uint64_t
{
  core::link_guard const held{mutex_};
  return dropped_;
}

i2c_medium::i2c_medium(
    std::size_t                 controller,
    i2c_link_port&              controller_port,
    core::sys_time              latency,
    std::vector<i2c_target_end> targets,
    std::size_t                 tap_capacity)
    : controller_port_{controller_port}, tap_{std::make_shared<i2c_tap_queue>(tap_capacity)}
{
  if(targets.empty())
  {
    throw std::invalid_argument("an I2C medium has at least one target");
  }
  for(auto const& [index, target] : std::views::enumerate(targets))
  {
    if(target.member == controller)
    {
      throw std::invalid_argument(
          std::format("I2C target {} is an I2C on another member than the controller's", index));
    }
    if(target.port == nullptr)
    {
      throw std::invalid_argument(std::format("I2C target {} has an I2C link port", index));
    }
    if(target.forth_lane == 0 or target.back_lane == 0)
    {
      throw std::invalid_argument(std::format("I2C target {} has lanes of 1 and up", index));
    }
    for(auto const& other : targets | std::views::take(index))
    {
      if(other.back_lane == target.back_lane)
      {
        throw std::invalid_argument(
            std::format("I2C target {} has an answer lane of its own on the controller", index));
      }
    }
  }
  for(auto const& [index, target] : std::views::enumerate(targets))
  {
    auto pair = std::make_shared<detail::i2c_pair>(
        static_cast<std::size_t>(index), controller, target, latency, tap_);
    pair->forth            = std::make_shared<detail::i2c_forth>(*pair);
    pair->back             = std::make_shared<detail::i2c_back>(*pair);
    pair->controller_proxy = std::make_shared<detail::i2c_controller_proxy>(*pair->forth);
    pair->target_proxy     = std::make_shared<detail::i2c_target_proxy>(*pair->back);
    pairs_.push_back(std::move(pair));
  }
}

i2c_medium::~i2c_medium() = default;

auto i2c_medium::links() const -> std::vector<std::shared_ptr<core::member_link>>
{
  std::vector<std::shared_ptr<core::member_link>> out;
  for(auto const& pair : pairs_)
  {
    out.push_back(std::shared_ptr<core::member_link>(pair, pair->forth.get()));
    out.push_back(std::shared_ptr<core::member_link>(pair, pair->back.get()));
  }
  return out;
}

void i2c_medium::attach_controller()
{
  std::size_t attached = 0;
  try
  {
    for(; attached < pairs_.size(); ++attached)
    {
      auto const& held = pairs_[attached];
      controller_port_.attach_target(
          std::shared_ptr<i2c_link_target>(held, held->controller_proxy.get()));
    }
  }
  catch(...)
  {
    while(attached > 0)
    {
      controller_port_.detach_target(*pairs_[--attached]->controller_proxy);
    }
    throw;
  }
}

void i2c_medium::attach_target(std::size_t index)
{
  auto const& held = pairs_.at(index);
  held->end.port->attach_controller(
      std::shared_ptr<i2c_link_controller>(held, held->target_proxy.get()));
}

void i2c_medium::give_back_controller() noexcept
{
  for(auto const& pair : pairs_)
  {
    pair->forth->complete_open();
    controller_port_.detach_target(*pair->controller_proxy);
  }
}

void i2c_medium::give_back_target(std::size_t index) noexcept
{
  auto& pair = *pairs_[index];
  pair.end.port->detach_controller(*pair.target_proxy);
  pair.back->give_back();
}

auto i2c_medium::tap() noexcept -> i2c_tap_queue&
{
  return *tap_;
}

auto i2c_medium::target_count() const noexcept -> std::size_t
{
  return pairs_.size();
}

auto i2c_medium::refused() const -> std::uint64_t
{
  return std::ranges::fold_left(
      pairs_ | std::views::transform([](auto const& pair) { return pair->forth->state().refused; }),
      std::uint64_t{}, std::plus{});
}

auto i2c_medium::owed(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->back->state().owed.size();
}

auto i2c_medium::owed_peak(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->owed_peak;
}

auto i2c_medium::tickets(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->forth->state().tickets.size();
}

auto i2c_medium::tickets_peak(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->tickets_peak;
}
}

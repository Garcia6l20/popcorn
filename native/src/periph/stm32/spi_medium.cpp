#include "spi_medium.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/link_guard.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace popcorn::periph::stm32
{
namespace detail
{
constexpr std::uint8_t forth_select   = 0;
constexpr std::uint8_t forth_deselect = 1;
constexpr std::uint8_t forth_frame    = 2;

struct spi_forth_post
{
  std::uint8_t   kind{};
  std::uint64_t  unit{};
  core::sys_time stamp{};
  std::uint32_t  word{};
  std::uint8_t   bits{};
  std::uint64_t  frame_ps{};

  friend auto operator==(spi_forth_post const&, spi_forth_post const&) -> bool = default;
};

struct spi_back_post
{
  std::uint64_t  unit{};
  core::sys_time stamp{};
  core::sys_time sent{};
  std::uint32_t  mosi{};
  std::uint32_t  miso{};
  std::uint8_t   bits{};
  std::uint8_t   dropped{};

  friend auto operator==(spi_back_post const&, spi_back_post const&) -> bool = default;
};

struct spi_ticket
{
  std::uint64_t unit{};
  std::uint64_t ticket{};
  std::uint8_t  bits{};

  friend auto operator==(spi_ticket const&, spi_ticket const&) -> bool = default;
};

struct spi_forth_state
{
  std::string                 kind{"spi_forth"};
  std::vector<spi_forth_post> mail_even;
  std::vector<spi_forth_post> mail_odd;
  std::vector<spi_forth_post> carried;
  std::uint64_t               window{};
  core::sys_time              sender_base{};
  std::uint64_t               unit{};
  std::uint8_t                level{};
  std::uint8_t                muted{};
  std::vector<spi_ticket>     tickets;
  std::vector<core::sys_time> resets;
  core::sys_time              last_barrier{};
  std::uint64_t               refused{};
  std::vector<spi_back_post>  undriven;

  friend auto operator==(spi_forth_state const&, spi_forth_state const&) -> bool = default;
};

struct spi_owed
{
  std::uint64_t  unit{};
  core::sys_time sent{};
  std::uint32_t  word{};
  std::uint8_t   bits{};
  core::sys_time end{};

  friend auto operator==(spi_owed const&, spi_owed const&) -> bool = default;
};

struct spi_back_state
{
  std::string                 kind{"spi_back"};
  std::vector<spi_back_post>  mail_even;
  std::vector<spi_back_post>  mail_odd;
  std::vector<spi_back_post>  carried;
  std::uint64_t               window{};
  core::sys_time              sender_base{};
  std::vector<spi_owed>       owed;
  std::vector<core::sys_time> resets;
  core::sys_time              last_barrier{};

  friend auto operator==(spi_back_state const&, spi_back_state const&) -> bool = default;
};

namespace
{
auto mask_of(std::uint32_t bits) -> std::uint32_t
{
  return bits >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << bits) - 1u;
}

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
        "spi link state", 0, "bytes",
        std::format("a state an SPI link of kind {} saved: {}", State{}.kind, failure.what()));
  }
  if(loaded.kind != State{}.kind)
  {
    throw config_error(
        "spi link state", 0, "kind",
        std::format("an SPI link state of kind {}, the bytes hold {}", State{}.kind, loaded.kind));
  }
  return loaded;
}
}

class spi_forth;
class spi_back;

class spi_master_proxy final : public spi_link_target
{
public:
  explicit spi_master_proxy(spi_forth& link) : link_{link}
  {}

  void linked(spi_link_port& port, core::clock const& clock) override
  {
    port_  = &port;
    clock_ = &clock;
  }

  void attached(spi_target_port& port) override
  {
    target_ = &port;
  }

  void detached() noexcept override
  {
    port_   = nullptr;
    clock_  = nullptr;
    target_ = nullptr;
  }

  void select(bool selected, vtime at) override;

  auto exchange(std::uint32_t, std::uint8_t bits, vtime) -> std::uint32_t override
  {
    return mask_of(bits);
  }

  auto exchange_deferred(std::uint32_t out, std::uint8_t bits, vtime at, std::uint64_t ticket)
      -> std::optional<std::uint32_t> override;

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto port() const noexcept -> spi_link_port*
  {
    return port_;
  }

  [[nodiscard]] auto target() const noexcept -> spi_target_port*
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
  spi_forth&         link_;
  spi_link_port*     port_{};
  core::clock const* clock_{};
  spi_target_port*   target_{};
};

class spi_slave_proxy final : public spi_link_controller
{
public:
  explicit spi_slave_proxy(spi_back& link) : link_{link}
  {}

  void linked(spi_controller_port& port, core::clock const&) override
  {
    port_ = &port;
  }

  void unlinked() noexcept override
  {
    port_ = nullptr;
  }

  void frame(std::uint32_t, std::uint32_t, vtime) override
  {}

  void
      unit_frame(std::uint64_t unit, std::uint32_t sent, std::uint32_t received, vtime at) override;
  void dropped(std::uint64_t unit, std::uint32_t sent, vtime at) override;

  [[nodiscard]] auto port() const noexcept -> spi_controller_port*
  {
    return port_;
  }

private:
  spi_back&            link_;
  spi_controller_port* port_{};
};

struct spi_pair
{
  spi_pair(
      std::size_t                    slave_index,
      std::size_t                    master,
      spi_link_port&                 master_port,
      spi_slave_end                  end,
      core::sys_time                 latency,
      std::shared_ptr<spi_tap_queue> tap);

  std::size_t                       index;
  std::size_t                       master;
  spi_link_port&                    master_port;
  spi_slave_end                     end;
  core::sys_time                    latency;
  std::shared_ptr<spi_tap_queue>    tap;
  std::shared_ptr<spi_forth>        forth;
  std::shared_ptr<spi_back>         back;
  std::shared_ptr<spi_master_proxy> master_proxy;
  std::shared_ptr<spi_slave_proxy>  slave_proxy;
  std::size_t                       owed_peak{};
  std::size_t                       tickets_peak{};
};

class spi_forth final : public core::member_link
{
public:
  explicit spi_forth(spi_pair& pair) : pair_{pair}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return pair_.master;
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
    auto& proxy        = *pair_.master_proxy;
    if(state_.muted == 0 or proxy.port() == nullptr)
    {
      return;
    }
    state_.muted   = 0;
    auto const low = not proxy.port()->unmute(proxy);
    if(low)
    {
      post_level(true, proxy.clock()->time());
    }
  }

  void sender_reset(core::sys_time at) override
  {
    if(state_.level != 0)
    {
      mailbox().push_back({.kind = forth_deselect, .unit = ++state_.unit, .stamp = at});
      state_.level = 0;
    }
    state_.tickets.clear();
    auto& proxy = *pair_.master_proxy;
    if(proxy.port() != nullptr)
    {
      proxy.port()->mute(proxy);
      state_.muted = 1;
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
    auto loaded = decoded<spi_forth_state>(in);
    state_      = std::move(loaded);
    auto& proxy = *pair_.master_proxy;
    if(proxy.port() == nullptr)
    {
      return;
    }
    if(state_.muted != 0)
    {
      proxy.port()->mute(proxy);
    }
    else
    {
      std::ignore = proxy.port()->unmute(proxy);
    }
  }

  void post_level(bool selected, vtime local)
  {
    if((state_.level != 0) == selected)
    {
      return;
    }
    state_.level = selected ? 1 : 0;
    mailbox().push_back(
        {.kind  = selected ? forth_select : forth_deselect,
         .unit  = ++state_.unit,
         .stamp = core::time_map{state_.sender_base}.to_system(local)});
  }

  void post_frame(
      std::uint32_t word,
      std::uint8_t  bits,
      vtime         local,
      std::uint64_t ticket,
      std::uint64_t frame_ps)
  {
    auto const unit = ++state_.unit;
    mailbox().push_back(
        {.kind     = forth_frame,
         .unit     = unit,
         .stamp    = core::time_map{state_.sender_base}.to_system(local),
         .word     = word,
         .bits     = bits,
         .frame_ps = frame_ps});
    state_.tickets.push_back({.unit = unit, .ticket = ticket, .bits = bits});
    pair_.tickets_peak = std::max(pair_.tickets_peak, state_.tickets.size());
  }

  void reply(spi_back_post const& post, core::time_map receiver)
  {
    auto const found = std::ranges::find(state_.tickets, post.unit, &spi_ticket::unit);
    auto&      proxy = *pair_.master_proxy;
    if(found == state_.tickets.end() or proxy.target() == nullptr)
    {
      ++state_.refused;
      return;
    }
    auto const ticket = found->ticket;
    state_.tickets.erase(found);
    auto const local = receiver.to_local(plus(post.stamp, pair_.latency.ps));
    proxy.target()->complete(
        ticket, post.miso, vtime{local.ps + proxy.offset()}, pair_.end.back_lane);
  }

  void complete_open() noexcept
  {
    auto& proxy   = *pair_.master_proxy;
    auto  tickets = std::exchange(state_.tickets, {});
    if(proxy.target() == nullptr)
    {
      return;
    }
    for(auto const& open : tickets)
    {
      try
      {
        proxy.target()->complete(
            open.ticket, mask_of(open.bits), proxy.clock()->since_power_on(), pair_.end.back_lane);
      }
      catch(...)
      {}
    }
  }

  void arm_attach()
  {
    state_.muted = 1;
    state_.level = 0;
  }

  [[nodiscard]] auto muted() const noexcept -> bool
  {
    return state_.muted != 0;
  }

  auto take_undriven() -> std::vector<spi_back_post>
  {
    return std::exchange(state_.undriven, {});
  }

  [[nodiscard]] auto state() const noexcept -> spi_forth_state const&
  {
    return state_;
  }

private:
  auto mailbox() -> std::vector<spi_forth_post>&
  {
    return state_.window % 2 == 0 ? state_.mail_even : state_.mail_odd;
  }

  void hand(spi_forth_post const& post, core::time_map receiver);

  spi_pair&       pair_;
  spi_forth_state state_;
};

class spi_back final : public core::member_link
{
public:
  explicit spi_back(spi_pair& pair) : pair_{pair}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return pair_.end.member;
  }

  [[nodiscard]] auto receiver() const noexcept -> std::size_t override
  {
    return pair_.master;
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
                       .member = pair_.master,
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
    state_ = decoded<spi_back_state>(in);
  }

  void owe(spi_owed owed)
  {
    state_.owed.push_back(owed);
    pair_.owed_peak = std::max(pair_.owed_peak, state_.owed.size());
  }

  void answer_frame(std::uint64_t unit, std::uint32_t sent, std::uint32_t received, vtime at)
  {
    auto const found = std::ranges::find(state_.owed, unit, &spi_owed::unit);
    if(found == state_.owed.end())
    {
      return;
    }
    auto const owed = *found;
    state_.owed.erase(found);
    mailbox().push_back(
        {.unit  = owed.unit,
         .stamp = core::time_map{state_.sender_base}.to_system(at),
         .sent  = owed.sent,
         .mosi  = sent,
         .miso  = received,
         .bits  = owed.bits});
  }

  void answer_dropped(std::uint64_t unit, vtime at)
  {
    auto const found = std::ranges::find(state_.owed, unit, &spi_owed::unit);
    if(found == state_.owed.end())
    {
      return;
    }
    auto const owed = *found;
    state_.owed.erase(found);
    mailbox().push_back(
        {.unit    = owed.unit,
         .stamp   = core::time_map{state_.sender_base}.to_system(at),
         .sent    = owed.sent,
         .mosi    = owed.word,
         .miso    = mask_of(owed.bits),
         .bits    = owed.bits,
         .dropped = 1});
  }

  [[nodiscard]] auto state() const noexcept -> spi_back_state const&
  {
    return state_;
  }

  static auto undriven(spi_owed const& owed) -> spi_back_post
  {
    return {
        .unit    = owed.unit,
        .stamp   = owed.end,
        .sent    = owed.sent,
        .mosi    = owed.word,
        .miso    = mask_of(owed.bits),
        .bits    = owed.bits,
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
  auto mailbox() -> std::vector<spi_back_post>&
  {
    return state_.window % 2 == 0 ? state_.mail_even : state_.mail_odd;
  }

  void show(spi_back_post const& post)
  {
    pair_.tap->push(
        {.slave   = pair_.index,
         .unit    = post.unit,
         .cs_pin  = pair_.end.cs_pin,
         .at      = post.sent,
         .reply   = post.stamp,
         .mosi    = post.mosi,
         .miso    = post.miso,
         .bits    = post.bits,
         .dropped = post.dropped != 0});
  }

  spi_pair&      pair_;
  spi_back_state state_;
};

void spi_forth::hand(spi_forth_post const& post, core::time_map receiver)
{
  auto* const port  = pair_.slave_proxy->port();
  auto const  reset = latest_after(state_.resets, post.stamp);
  auto const  start = plus(post.stamp, pair_.latency.ps);
  auto const  lane  = pair_.end.forth_lane;
  if(post.kind != forth_frame)
  {
    if(port == nullptr)
    {
      return;
    }
    auto const due = receiver.to_local(reset ? std::max(*reset, start) : start);
    if(post.kind == forth_select)
    {
      port->select(due, lane, post.unit);
    }
    else
    {
      port->deselect(due, lane, post.unit);
    }
    return;
  }
  auto const frame_end = plus(start, static_cast<std::int64_t>(post.frame_ps));
  auto const owed      = spi_owed{
      .unit = post.unit,
      .sent = post.stamp,
      .word = post.word,
      .bits = post.bits,
      .end  = reset ? std::max(*reset, frame_end) : frame_end};
  if(port == nullptr)
  {
    state_.undriven.push_back(spi_back::undriven(owed));
    return;
  }
  if(reset)
  {
    port->drop(receiver.to_local(owed.end), lane, post.unit);
    pair_.back->owe(owed);
    return;
  }
  port->frame(post.word, post.bits, post.frame_ps, receiver.to_local(start), lane, post.unit);
  pair_.back->owe(owed);
}

void spi_master_proxy::select(bool selected, vtime at)
{
  if(link_.muted() or clock_ == nullptr)
  {
    return;
  }
  link_.post_level(selected, vtime{at.ps - offset()});
}

auto spi_master_proxy::exchange_deferred(
    std::uint32_t out,
    std::uint8_t  bits,
    vtime         at,
    std::uint64_t ticket) -> std::optional<std::uint32_t>
{
  if(link_.muted() or clock_ == nullptr)
  {
    return mask_of(bits);
  }
  link_.post_frame(out, bits, vtime{at.ps - offset()}, ticket, port_->frame_time());
  return std::nullopt;
}

void spi_slave_proxy::unit_frame(
    std::uint64_t unit,
    std::uint32_t sent,
    std::uint32_t received,
    vtime         at)
{
  link_.answer_frame(unit, sent, received, at);
}

void spi_slave_proxy::dropped(std::uint64_t unit, std::uint32_t, vtime at)
{
  link_.answer_dropped(unit, at);
}

spi_pair::spi_pair(
    std::size_t                    slave_index,
    std::size_t                    master_member,
    spi_link_port&                 port,
    spi_slave_end                  slave,
    core::sys_time                 link_latency,
    std::shared_ptr<spi_tap_queue> frames)
    : index{slave_index}, master{master_member}, master_port{port}, end{std::move(slave)},
      latency{link_latency}, tap{std::move(frames)}
{}
}

spi_tap_queue::spi_tap_queue(std::size_t capacity) : capacity_{capacity}
{}

void spi_tap_queue::push(spi_tap_frame const& frame)
{
  core::link_guard const held{mutex_};
  if(frames_.size() == capacity_)
  {
    frames_.pop_front();
    ++dropped_;
  }
  frames_.push_back(frame);
}

auto spi_tap_queue::take() -> std::vector<spi_tap_frame>
{
  core::link_guard const     held{mutex_};
  std::vector<spi_tap_frame> out{frames_.begin(), frames_.end()};
  frames_.clear();
  return out;
}

auto spi_tap_queue::dropped() const -> std::uint64_t
{
  core::link_guard const held{mutex_};
  return dropped_;
}

spi_medium::spi_medium(
    std::size_t                master,
    spi_link_port&             master_port,
    core::sys_time             latency,
    std::vector<spi_slave_end> slaves,
    std::size_t                tap_capacity)
    : master_port_{master_port}, tap_{std::make_shared<spi_tap_queue>(tap_capacity)}
{
  if(slaves.empty())
  {
    throw std::invalid_argument("an SPI medium has at least one slave");
  }
  for(auto const& [index, slave] : std::views::enumerate(slaves))
  {
    if(slave.member == master)
    {
      throw std::invalid_argument(
          std::format("SPI slave {} is an SPI on another member than the master's", index));
    }
    if(slave.port == nullptr)
    {
      throw std::invalid_argument(std::format("SPI slave {} has an SPI link port", index));
    }
    if(slave.forth_lane == 0 or slave.back_lane == 0)
    {
      throw std::invalid_argument(std::format("SPI slave {} has lanes of 1 and up", index));
    }
    for(auto const& other : slaves | std::views::take(index))
    {
      if(other.back_lane == slave.back_lane)
      {
        throw std::invalid_argument(
            std::format("SPI slave {} has a reply lane of its own on the master", index));
      }
    }
  }
  for(auto&& [index, slave] : std::views::enumerate(slaves))
  {
    auto pair = std::make_shared<detail::spi_pair>(
        static_cast<std::size_t>(index), master, master_port, std::move(slave), latency, tap_);
    pair->forth        = std::make_shared<detail::spi_forth>(*pair);
    pair->back         = std::make_shared<detail::spi_back>(*pair);
    pair->master_proxy = std::make_shared<detail::spi_master_proxy>(*pair->forth);
    pair->slave_proxy  = std::make_shared<detail::spi_slave_proxy>(*pair->back);
    pairs_.push_back(std::move(pair));
  }
}

spi_medium::~spi_medium() = default;

auto spi_medium::links() const -> std::vector<std::shared_ptr<core::member_link>>
{
  std::vector<std::shared_ptr<core::member_link>> out;
  for(auto const& pair : pairs_)
  {
    out.push_back(std::shared_ptr<core::member_link>(pair, pair->forth.get()));
    out.push_back(std::shared_ptr<core::member_link>(pair, pair->back.get()));
  }
  return out;
}

void spi_medium::attach_master()
{
  std::size_t attached = 0;
  try
  {
    for(; attached < pairs_.size(); ++attached)
    {
      auto const& held = pairs_[attached];
      auto&       pair = *held;
      pair.forth->arm_attach();
      master_port_.attach_target(
          std::shared_ptr<spi_link_target>(held, pair.master_proxy.get()), pair.end.cs_port,
          pair.end.cs_pin);
      master_port_.mute(*pair.master_proxy);
    }
  }
  catch(...)
  {
    for(std::size_t index = 0; index < attached; ++index)
    {
      master_port_.detach_target(*pairs_[index]->master_proxy);
    }
    throw;
  }
}

void spi_medium::attach_slave(std::size_t index)
{
  auto const& held = pairs_.at(index);
  held->end.port->attach_controller(
      std::shared_ptr<spi_link_controller>(held, held->slave_proxy.get()));
}

void spi_medium::give_back_master() noexcept
{
  for(auto const& pair : pairs_)
  {
    pair->forth->complete_open();
    master_port_.detach_target(*pair->master_proxy);
  }
}

void spi_medium::give_back_slave(std::size_t index) noexcept
{
  auto& pair = *pairs_[index];
  pair.end.port->detach_controller(*pair.slave_proxy);
  pair.back->give_back();
}

auto spi_medium::tap() noexcept -> spi_tap_queue&
{
  return *tap_;
}

auto spi_medium::slave_count() const noexcept -> std::size_t
{
  return pairs_.size();
}

auto spi_medium::refused() const -> std::uint64_t
{
  return std::ranges::fold_left(
      pairs_ | std::views::transform([](auto const& pair) { return pair->forth->state().refused; }),
      std::uint64_t{}, std::plus{});
}

auto spi_medium::owed(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->back->state().owed.size();
}

auto spi_medium::owed_peak(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->owed_peak;
}

auto spi_medium::tickets(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->forth->state().tickets.size();
}

auto spi_medium::tickets_peak(std::size_t index) const -> std::size_t
{
  return pairs_.at(index)->tickets_peak;
}
}

#include <popcorn/plugin/plugin.hpp>

#include <atomic>
#include <memory>

extern "C"
{
  [[gnu::visibility("default")]] std::atomic<int> good_init_calls{0};
}

POPCORN_PLUGIN(good, r)
{
  ++good_init_calls;
  r.add_model("fake", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
    return std::shared_ptr<popcorn::periph::model>{};
  });
}

#include <popcorn/plugin/plugin.hpp>

#include <memory>
#include <stdexcept>

POPCORN_PLUGIN(throws, r)
{
  r.add_model("fake", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
    return std::shared_ptr<popcorn::periph::model>{};
  });
  throw std::runtime_error("throws fixture: init failed");
}

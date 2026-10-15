#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <memory>

POPCORN_PLUGIN(writes_global, r)
{
  popcorn::periph::register_model(
      "WRITES_GLOBAL", "*", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
        return std::shared_ptr<popcorn::periph::model>{};
      });
  r.add_model("fake", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
    return std::shared_ptr<popcorn::periph::model>{};
  });
}

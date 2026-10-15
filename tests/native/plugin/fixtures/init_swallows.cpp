#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <memory>

POPCORN_PLUGIN(init_swallows, r)
{
  try
  {
    popcorn::periph::register_model(
        "INIT_SWALLOWS", "*", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
          return std::shared_ptr<popcorn::periph::model>{};
        });
  }
  catch(...)
  {}
  r.add_model("fake", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
    return std::shared_ptr<popcorn::periph::model>{};
  });
}

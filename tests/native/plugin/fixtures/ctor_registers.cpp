#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <memory>

namespace
{
struct registers_on_load
{
  registers_on_load()
  {
    popcorn::periph::register_model(
        "CTOR_REGISTERS", "*",
        [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
          return std::shared_ptr<popcorn::periph::model>{};
        });
  }
};

registers_on_load const on_load;
}

POPCORN_PLUGIN(ctor_registers, r)
{
  r.add_model("fake", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
    return std::shared_ptr<popcorn::periph::model>{};
  });
}

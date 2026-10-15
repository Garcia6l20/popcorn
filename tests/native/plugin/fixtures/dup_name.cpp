#include <popcorn/plugin/plugin.hpp>

#include <memory>

namespace
{
auto none(popcorn::periph::model_context&, popcorn::peripheral_desc const&)
    -> std::shared_ptr<popcorn::periph::model>
{
  return {};
}
}

POPCORN_PLUGIN(dup_name, r)
{
  r.add_model("fake", none);
  r.add_model("fake", none);
}

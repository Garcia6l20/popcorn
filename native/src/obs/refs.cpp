#include "refs.hpp"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>

namespace popcorn::obs
{
auto reference_of(image::image const& loaded, image_kind kind, std::string_view path) -> image_ref
{
  image_ref ref{
      .kind   = kind,
      .path   = path.empty() ? std::string{}
                             : std::filesystem::absolute(path).lexically_normal().string(),
      .digest = loaded.digest};
  auto executable = loaded.segments | std::views::filter(&image::segment::executable);
  if(not std::ranges::empty(executable))
  {
    ref.base = std::ranges::min(executable | std::views::transform(&image::segment::start));
    ref.end  = std::ranges::max(executable | std::views::transform([](image::segment const& held) {
                                 return held.start + held.memory_size;
                                }));
  }
  return ref;
}
}

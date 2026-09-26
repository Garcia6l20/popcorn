#pragma once

namespace popcorn::bind::detail
{
/**
 * @brief Registers the nanobind translator turning each popcorn exception into its class of
 *        `popcorn._errors`, with the context fields as attributes. Other exceptions, a subclass
 *        of popcorn::error declared outside the core included, pass through to the translators
 *        registered before it: nanobind keeps one translator list for every extension.
 */
void register_error_translator();
}

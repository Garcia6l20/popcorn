/**
 * @brief Default ThreadSanitizer options of native test programs of the tsan variant: the
 *        suppression file POPCORN_TSAN_SUPPRESSIONS. ThreadSanitizer reads these first, then
 *        TSAN_OPTIONS flag by flag, so a shell value adds to them and overrides only the flags
 *        it names.
 */
extern "C" auto __tsan_default_options() -> char const*
{
  return "suppressions=" POPCORN_TSAN_SUPPRESSIONS;
}

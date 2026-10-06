#include <dlfcn.h>

#include <csetjmp>
#include <cstdlib>

namespace
{
using siglongjmp_fn = void (*)(sigjmp_buf, int);

auto libc_siglongjmp() -> siglongjmp_fn
{
  static auto const found = [] {
    void* const libc = dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD);
    if(libc == nullptr)
    {
      std::abort();
    }
    return reinterpret_cast<siglongjmp_fn>(dlsym(libc, "siglongjmp"));
  }();
  return found;
}
}

/**
 * @brief siglongjmp of libc, bypassing the ThreadSanitizer interceptor. Linked into native test
 *        programs of the tsan variant only. With glibc 2.44 the interceptor aborts every
 *        siglongjmp with "can't find longjmp buf", GCC 16.2 and clang alike, a six-line C
 *        program reproduces it. Unicorn leaves its TCG loop through siglongjmp, and the frames it
 *        skips are Unicorn's own, which ThreadSanitizer does not instrument.
 */
extern "C" void siglongjmp(sigjmp_buf env, int value) noexcept
{
  libc_siglongjmp()(env, value);
  __builtin_unreachable();
}

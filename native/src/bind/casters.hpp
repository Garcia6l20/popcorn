/**
 * @file
 * @brief The nanobind type casters every binding translation unit using nanobind shares, through
 *        a header each of them includes. A unit lacking one falls back to nanobind's generic
 *        caster for that type: an ODR violation, and a call that converts no argument and throws
 *        nanobind::cast_error.
 */
#pragma once

#include <nanobind/stl/string.h>

#include <string>
#include <utility>

namespace popcorn::bind::detail
{
/**
 * @brief Bytes read from a file that are text in Python, decoded as UTF-8 with surrogateescape,
 *        so bytes that are no UTF-8 survive a round trip and never raise.
 */
struct lenient_text
{
  /** @brief The bytes. */
  std::string bytes{};
};
}

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

template <> struct type_caster<popcorn::bind::detail::lenient_text>
{
  NB_TYPE_CASTER(popcorn::bind::detail::lenient_text, const_name("str"))

  auto from_python(handle src, uint8_t, cleanup_list*) noexcept -> bool
  {
    if(not PyUnicode_Check(src.ptr()))
    {
      return false;
    }
    PyObject* const encoded = PyUnicode_AsEncodedString(src.ptr(), "utf-8", "surrogateescape");
    if(encoded == nullptr)
    {
      PyErr_Clear();
      return false;
    }
    value.bytes = std::string(
        PyBytes_AS_STRING(encoded), static_cast<std::size_t>(PyBytes_GET_SIZE(encoded)));
    Py_DECREF(encoded);
    return true;
  }

  static auto
      from_cpp(popcorn::bind::detail::lenient_text const& text, rv_policy, cleanup_list*) noexcept
      -> handle
  {
    return PyUnicode_DecodeUTF8(
        text.bytes.data(), static_cast<Py_ssize_t>(text.bytes.size()), "surrogateescape");
  }
};

NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)

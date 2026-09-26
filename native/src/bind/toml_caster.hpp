#pragma once

#include <reflex/serde/toml_value.hpp>

#include <nanobind/nanobind.h>

#include <string>

namespace nanobind::detail
{
/**
 * @brief Converts a TOML value to the Python value tomllib gives for it and back: int, float,
 *        bool, str, list and dict. A date-time is a string, as reflex serde reads it.
 */
template <> struct type_caster<reflex::serde::toml::value>
{
  NB_TYPE_CASTER(reflex::serde::toml::value, const_name("object"))

  /**
   * @brief Reads a Python value.
   * @param src Python object.
   * @return Whether it maps to a TOML value.
   */
  bool from_python(handle src, uint8_t flags, cleanup_list* cleanup) noexcept
  {
    namespace toml = reflex::serde::toml;
    if(PyBool_Check(src.ptr()))
    {
      value = toml::value{src.ptr() == Py_True};
      return true;
    }
    if(PyLong_Check(src.ptr()))
    {
      auto const number = PyLong_AsLongLong(src.ptr());
      if(number == -1 and PyErr_Occurred() != nullptr)
      {
        PyErr_Clear();
        return false;
      }
      value = toml::value{toml::integer{number}};
      return true;
    }
    if(PyFloat_Check(src.ptr()))
    {
      value = toml::value{toml::number{PyFloat_AsDouble(src.ptr())}};
      return true;
    }
    if(PyUnicode_Check(src.ptr()))
    {
      value = toml::value{toml::string{nanobind::cast<std::string>(src)}};
      return true;
    }
    if(PyList_Check(src.ptr()))
    {
      toml::array items;
      for(auto const item : borrow<list>(src))
      {
        type_caster element;
        if(not element.from_python(item, flags, cleanup))
        {
          return false;
        }
        items.push_back(std::move(element.value));
      }
      value = toml::value{std::move(items)};
      return true;
    }
    if(PyDict_Check(src.ptr()))
    {
      toml::object table;
      for(auto const [key, item] : borrow<dict>(src))
      {
        type_caster element;
        if(not PyUnicode_Check(key.ptr()) or not element.from_python(item, flags, cleanup))
        {
          return false;
        }
        table.emplace(nanobind::cast<std::string>(key), std::move(element.value));
      }
      value = toml::value{std::move(table)};
      return true;
    }
    return false;
  }

  /**
   * @brief Builds the Python value.
   * @param src TOML value.
   * @return New reference, or null with a Python error set for a null value.
   */
  static handle from_cpp(reflex::serde::toml::value const& src, rv_policy, cleanup_list*) noexcept
  {
    namespace toml = reflex::serde::toml;
    if(src.is<toml::boolean>())
    {
      return PyBool_FromLong(src.as<toml::boolean>() ? 1 : 0);
    }
    if(src.is<toml::integer>())
    {
      return PyLong_FromLongLong(src.as<toml::integer>());
    }
    if(src.is<toml::number>())
    {
      return PyFloat_FromDouble(src.as<toml::number>());
    }
    if(src.is<toml::string>())
    {
      auto const& text = src.as<toml::string>();
      return PyUnicode_FromStringAndSize(text.data(), static_cast<Py_ssize_t>(text.size()));
    }
    if(src.is_array())
    {
      auto const& items = src.as<toml::array>();
      auto*       out   = PyList_New(static_cast<Py_ssize_t>(items.size()));
      for(Py_ssize_t index = 0; auto const& item : items)
      {
        auto const element = from_cpp(item, rv_policy::copy, nullptr);
        if(not element.is_valid())
        {
          Py_DECREF(out);
          return {};
        }
        PyList_SET_ITEM(out, index++, element.ptr());
      }
      return out;
    }
    if(src.is_object())
    {
      auto* out = PyDict_New();
      for(auto const& [key, item] : src.as<toml::object>())
      {
        auto const element = from_cpp(item, rv_policy::copy, nullptr);
        if(not element.is_valid() or PyDict_SetItemString(out, key.c_str(), element.ptr()) != 0)
        {
          Py_XDECREF(element.ptr());
          Py_DECREF(out);
          return {};
        }
        Py_DECREF(element.ptr());
      }
      return out;
    }
    PyErr_SetString(PyExc_ValueError, "a TOML value holds nothing");
    return {};
  }
};
}

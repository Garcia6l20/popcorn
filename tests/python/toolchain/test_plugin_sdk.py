"""The plugin SDK staged beside the built popcorn package: headers, runtime archive, sdk.json."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest
from popcorn import _core

from popcorn_build.plugin_sdk import main, sdk_headers

ROOT = Path(__file__).resolve().parents[3]


def staged_sdk() -> Path:
    sdk = Path(_core.__file__).resolve().parent / "sdk"
    if not (sdk / "sdk.json").is_file():
        pytest.skip(f"no plugin SDK in {sdk.parent}")
    return sdk


def test_sdk_json_names_the_compiler_popcorn_was_built_with() -> None:
    fields = json.loads((staged_sdk() / "sdk.json").read_text())
    version = subprocess.run(
        [fields["compiler"], "-dumpfullversion"], capture_output=True, text=True, check=True
    ).stdout.strip()
    assert fields["compiler_version"] == version
    tag = _core._host_abi_tag()  # pyright: ignore[reportPrivateUsage]
    (built_with,) = (part[len("gcc ") :] for part in tag.split(";") if part.startswith("gcc "))
    assert fields["compiler_ident"] == built_with
    assert {"-std=gnu++26", "-freflection", "-fcontracts", "-fabi-version=21"} <= set(
        fields["cxx_flags"]
    )
    assert "-Werror" not in fields["cxx_flags"]
    assert fields["ext_suffix"] == Path(_core.__file__).name.removeprefix("_core")


def test_every_sdk_header_is_staged_with_its_content() -> None:
    sdk = staged_sdk()
    deps = sdk.parents[1] / "deps" / "src"
    headers = sdk_headers(
        ROOT / "native" / "include",
        ROOT / "third_party" / "reflex",
        deps / "nanobind",
        deps / "robin-map",
    )
    assert any(relative == "popcorn/plugin/bindings.hpp" for relative, _ in headers)
    assert any(relative == "nanobind/nanobind.h" for relative, _ in headers)
    for relative, source in headers:
        assert (sdk / "include" / relative).read_bytes() == source.read_bytes(), relative
    assert (sdk / "include" / "popcorn" / "plugin" / "build_info.hpp").is_file()
    assert (sdk / "nanobind" / "libnanobind-runtime.a").is_file()


def test_main_rewrites_sdk_json_only_when_its_text_changes(tmp_path: Path) -> None:
    output = tmp_path / "sdk.json"
    fields = {
        "version": "0.1.0.dev0",
        "variant": "release",
        "compiler": "/nonexistent/g++",
        "cxx_flags": ["-fPIC"],
        "defines": ["NDEBUG"],
        "link_flags": [],
        "ext_suffix": ".so",
    }
    assert main([str(output), json.dumps(fields)]) == 0
    written = output.stat().st_mtime_ns
    assert main([str(output), json.dumps(fields)]) == 0
    assert output.stat().st_mtime_ns == written
    assert json.loads(output.read_text())["compiler_version"] == ""
    assert main([str(output), json.dumps({**fields, "cxx_flags": ["-fPIC", "-O2"]})]) == 0
    assert json.loads(output.read_text())["cxx_flags"] == ["-fPIC", "-O2"]

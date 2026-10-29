#!/usr/bin/env bash
set -euo pipefail

readonly SIZE_LIMIT=350000000
readonly VOLUME=popcorn-smoke-cache

image=${1:?usage: docker/smoke.sh IMAGE}
root=$(cd "$(dirname "$0")/.." && pwd)
examples=$root/examples
tmp=$(mktemp -d)
failed=()

cleanup() {
    docker run --rm --user 0 -v "$tmp:/t" "$image" find /t -mindepth 1 -delete >/dev/null 2>&1 || true
    rm -rf "$tmp"
    docker volume rm -f "$VOLUME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

check() {
    local name=$1
    shift
    local log=$tmp/$name.log
    if "$@" >"$log" 2>&1; then
        echo "ok   $name"
    else
        echo "FAIL $name"
        sed 's/^/     /' "$log"
        failed+=("$name")
    fi
}

writable_copy() {
    local dest=$tmp/$1
    cp -r "$examples/$2" "$dest"
    chmod -R a+rwX "$dest"
    echo "$dest"
}

import_version() {
    local out
    out=$(docker run --rm "$image" python -c "import popcorn; print(popcorn.version())")
    echo "$out"
    [[ $out == *"(unicorn "* && $out != *dirty* ]]
}

gpio_arbitrary_uid() {
    local work
    work=$(writable_copy gpio-4242 stm32u575/gpio)
    docker run --rm --user 4242:4242 -v "$work:/work" "$image" pytest -q
    test -d "$work/.pytest_cache/d/popcorn/boot"
}

gpio_default_uid_named_cache() {
    local work
    work=$(writable_copy gpio-default stm32u575/gpio)
    docker volume rm -f "$VOLUME" >/dev/null
    docker run --rm -v "$work:/work" -v "$VOLUME:/cache" "$image" pytest -q
    docker run --rm -v "$VOLUME:/cache" "$image" sh -c 'find /cache/popcorn/mcu -name "STM32U575QGIx*.json" | grep .'
}

h573_read_only() {
    docker run --rm --user "$(id -u):$(id -g)" -v "$examples/stm32h573/usart:/work:ro" "$image" \
        pytest -q -p no:cacheprovider
}

script_region() {
    local snippet=$root/tests/docs/guide/test_first_machine.py
    {
        sed -n '1,/^from popcorn.units/p' "$snippet" | grep -v '^import pytest$'
        echo 'ELF = Path("/work/prebuilt/usart_echo.elf")'
        sed -n '/\[start:describe\]/,/\[end:describe\]/p' "$snippet"
        sed -n '/\[start:script\]/,/\[end:script\]/p' "$snippet"
    } >"$tmp/script.py"
    local out
    out=$(docker run --rm -i -v "$examples/stm32u575/usart:/work:ro" "$image" python - <"$tmp/script.py")
    echo "$out"
    [[ $out == "popcorn usart example, 115200 baud"* ]]
}

one_libstdcxx() {
    docker run --rm -i -v "$examples/stm32u575/gpio:/work:ro" "$image" python - <<'EOF'
from pathlib import Path

import popcorn

description = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(Path("/work/prebuilt/gpio_button.elf"))],
)
machine = description.machine()
machine.reset()
paths = sorted({line.split()[-1] for line in open("/proc/self/maps") if "libstdc++" in line})
print(paths)
assert len(paths) == 1 and paths[0].startswith("/usr/local/lib/"), paths
EOF
}

no_compiler() {
    local name
    for name in g++ gcc arm-none-eabi-gcc; do
        if docker run --rm "$image" sh -c "command -v $name"; then
            echo "$name found"
            return 1
        fi
    done
}

default_uid() {
    local uid
    uid=$(docker run --rm "$image" id -u)
    echo "$uid"
    [[ $uid == 1000 ]]
}

licence_files() {
    docker run --rm "$image" sh -c '
        cd /usr/share/doc/popcorn
        ls LICENSE NOTICE IMAGE_NOTICE LICENSES/unicorn-COPYING LICENSES/STM32_open_pin_data-LICENSE \
            gcc-runtime/COPYING3 gcc-runtime/COPYING.RUNTIME
        ls /usr/local/lib/python3.14/site-packages/uv-*.dist-info/licenses/LICENSE-MIT \
            /usr/local/lib/python3.14/site-packages/uv-*.dist-info/licenses/LICENSE-APACHE'
}

image_size() {
    local size
    size=$(docker image inspect --format '{{.Size}}' "$image")
    echo "$size bytes, limit $SIZE_LIMIT"
    ((size <= SIZE_LIMIT))
}

derived_uv() {
    docker run --rm --user 0 "$image" uv pip install --dry-run pytest-xdist
}

check import_version import_version
check gpio_arbitrary_uid gpio_arbitrary_uid
check gpio_default_uid_named_cache gpio_default_uid_named_cache
check h573_read_only h573_read_only
check script_region script_region
check one_libstdcxx one_libstdcxx
check no_compiler no_compiler
check default_uid default_uid
check licence_files licence_files
check image_size image_size
check derived_uv derived_uv

if ((${#failed[@]})); then
    echo "failed: ${failed[*]}"
    exit 1
fi
echo "all checks passed"

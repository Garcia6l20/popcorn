FROM gcc:16.2.0@sha256:07280eb2e744f1b7a2d8d4a3894c089ea07266c109758afc388a634bd098e4eb AS toolchain
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
COPY --from=ghcr.io/astral-sh/uv:0.13.1@sha256:4a1f3666816dafadd20ceb71eac9a7d56d7ef30279f7d64048519ab901ada336 /uv /usr/local/bin/uv
ENV UV_PYTHON_INSTALL_DIR=/opt/python
RUN uv python install 3.14.8 && uv venv /opt/build --python 3.14.8 \
    && uv pip install --no-cache --python /opt/build/bin/python pcons==0.31.0

FROM toolchain AS unicorn
RUN --mount=type=bind,source=.git/modules/third_party/unicorn,target=/raw,rw \
    git config --file /raw/config --unset core.worktree \
    && git init -q /src/third_party/unicorn \
    && git -C /src/third_party/unicorn fetch -q --depth 1 --no-tags /raw "$(git --git-dir=/raw rev-parse HEAD)" \
    && git -C /src/third_party/unicorn checkout -q --detach FETCH_HEAD \
    && git -C /src/third_party/unicorn repack -q -a -d -f --threads=1 \
    && rm -r /src/third_party/unicorn/.git/index /src/third_party/unicorn/.git/logs \
       /src/third_party/unicorn/.git/FETCH_HEAD

FROM toolchain AS deps
WORKDIR /src
COPY pyproject.toml deps.toml ./
COPY popcorn_build popcorn_build
COPY --from=unicorn /src/third_party/unicorn third_party/unicorn
RUN git -C third_party/unicorn read-tree HEAD \
    && test -z "$(git -C third_party/unicorn status --porcelain)" \
    && mkdir -p third_party/reflex/core/include \
    && CMAKE_BUILD_PARALLEL_LEVEL=4 PCONS_BUILD_DIR=/src/build/wheel PCONS_VARIANT=release \
       PCONS_VARS='{"POPCORN_BUILD_TESTS":"OFF","POPCORN_BUILD_EXAMPLES":"OFF"}' \
       /opt/build/bin/python -c "import popcorn_build.requirements" \
    && rm -r third_party/reflex

FROM deps AS wheel
ARG JOBS=4
COPY pcons-build.py LICENSE NOTICE ./
COPY LICENSES LICENSES
COPY native native
COPY python python
COPY third_party/reflex third_party/reflex
RUN CMAKE_BUILD_PARALLEL_LEVEL=${JOBS} uv build --wheel --no-build-isolation \
    --python /opt/build/bin/python -C jobs=${JOBS} -o /out

FROM python:3.14.10-slim-trixie@sha256:3cabfd7d7aaf394e6040e79fa1eec6ca1c52b74ed9188650472695d9a9326999 AS runtime
COPY --from=toolchain /usr/local/lib64/libstdc++.so.6.0.36 /usr/local/lib64/libgcc_s.so.1 \
     /usr/local/lib64/libatomic.so.1.2.0 /usr/local/lib/
RUN ldconfig
RUN pip install --no-cache-dir uv==0.13.1
RUN --mount=type=bind,from=wheel,source=/out,target=/wheels \
    --mount=type=bind,source=docker/constraints.txt,target=/constraints.txt \
    uv venv /opt/venv --python /usr/local/bin/python3.14 \
    && uv pip install --no-cache --compile-bytecode --python /opt/venv/bin/python \
       -c /constraints.txt "$(ls /wheels/popcorn-*.whl)[pytest]"
RUN mkdir -p /usr/share/doc/popcorn \
    && cp -r /opt/venv/lib/python3.14/site-packages/popcorn-*.dist-info/licenses/. /usr/share/doc/popcorn/
COPY docker/IMAGE_NOTICE /usr/share/doc/popcorn/
COPY docker/licenses/gcc /usr/share/doc/popcorn/gcc-runtime
RUN useradd --create-home --uid 1000 popcorn && mkdir -p /work /cache \
    && chown popcorn /work && chmod 1777 /cache
ENV VIRTUAL_ENV=/opt/venv PATH=/opt/venv/bin:$PATH XDG_CACHE_HOME=/cache UV_CACHE_DIR=/cache/uv \
    UV_PYTHON_DOWNLOADS=never PYTHONUNBUFFERED=1
USER popcorn
WORKDIR /work
VOLUME /cache
CMD ["pytest"]
ARG POPCORN_REVISION=unknown
ARG POPCORN_VERSION=0.1.0.dev0
LABEL org.opencontainers.image.title="popcorn" \
      org.opencontainers.image.description="STM32 firmware emulation and testing with popcorn and pytest" \
      org.opencontainers.image.source="https://github.com/Garcia6l20/popcorn" \
      org.opencontainers.image.documentation="https://garcia6l20.github.io/popcorn/" \
      org.opencontainers.image.revision="${POPCORN_REVISION}" \
      org.opencontainers.image.version="${POPCORN_VERSION}" \
      org.opencontainers.image.licenses="MIT AND GPL-2.0-only AND LGPL-2.1-or-later AND BSD-3-Clause AND Apache-2.0 AND GPL-3.0-or-later WITH GCC-exception-3.1 AND (MIT OR Apache-2.0)"

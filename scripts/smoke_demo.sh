#!/usr/bin/env bash
set -euo pipefail

# Headless smoke demo for environments without CMake installed.
# It builds the scripted CLI demo and NDJSON stream demo directly with the
# system C++17 compiler, then runs both with bounded input/output.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build/smoke"
cxx="${CXX:-c++}"

mkdir -p "${build_dir}"

common_flags=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -I"${repo_root}/include")
common_src=("${repo_root}/src/order_book.cpp" "${repo_root}/src/matching_engine.cpp")

echo "[smoke] compiler: $(${cxx} --version | head -n 1)"
echo "[smoke] building tachyon demo"
"${cxx}" "${common_flags[@]}" "${repo_root}/apps/main.cpp" "${common_src[@]}" -o "${build_dir}/tachyon"

echo "[smoke] building tachyon_stream demo"
"${cxx}" "${common_flags[@]}" "${repo_root}/apps/stream_demo.cpp" "${common_src[@]}" -o "${build_dir}/tachyon_stream"

echo "[smoke] running scripted demo"
"${build_dir}/tachyon" >"${build_dir}/tachyon_demo.out"
grep -q "FINAL STATS" "${build_dir}/tachyon_demo.out"

echo "[smoke] running bounded stream demo"
stream_out="${build_dir}/stream.ndjson"
"${build_dir}/tachyon_stream" 5 2 0 >"${stream_out}"
line_count="$(wc -l <"${stream_out}" | tr -d ' ')"
test "${line_count}" = "5"
grep -q '"event":"order"' "${stream_out}"

echo "[smoke] ok: demo ran and stream emitted ${line_count} NDJSON events"

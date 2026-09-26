#!/usr/bin/env bash
# Run from the repository root. No hardware access, VM startup or installation.
set -eu
compiler="${CXX:-clang++}"
task_output="$(mktemp -d /tmp/ngreen-static.XXXXXX)"
printf 'Diagnostic directory: %s\n' "$task_output"
failed=0
for source in NootedGreen/*.cpp; do
    # Retired TCP logger is not part of the Xcode Sources build phase.
    if [[ "$source" == NootedGreen/kern_netdbg.cpp ]]; then
        printf 'EXCLUDED (not built): %s\n' "$source"
        continue
    fi
    if "$compiler" --target=x86_64-apple-macos13 -std=c++14 \
        -fsyntax-only -ffreestanding -fno-builtin \
        -DKERNEL=1 -DKERNEL_PRIVATE=1 -DMODULE_VERSION=100 \
        -DPRODUCT_NAME=NootedGreen -D__MAC_OS_X_VERSION_MIN_REQUIRED=130000 \
        -I. -ILilu.kext/Contents/Resources -IMacKernelSDK/Headers \
        "$source" > "$task_output/${source##*/}.log" 2>&1; then
        printf 'PASS syntax: %s\n' "$source"
    else
        printf 'FAIL syntax: %s (see diagnostic directory)\n' "$source"
        failed=1
    fi
done
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/guc_ring_test.cpp -o "$task_output/guc-ring-test" && \
    "$task_output/guc-ring-test"; then
    printf 'PASS offline ring tests\n'
else
    failed=1
fi
git diff --check || failed=1
exit "$failed"

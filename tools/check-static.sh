#!/usr/bin/env bash
# Run from the repository root. No hardware access, VM startup or installation.
set -eu
compiler="${CXX:-clang++}"
task_output="$(mktemp -d /tmp/ngreen-static.XXXXXX)"
printf 'Diagnostic directory: %s\n' "$task_output"
failed=0
for source in NootedGreen/*.cpp; do
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
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/gpu_capabilities_test.cpp -o "$task_output/gpu-capabilities-test" && \
    "$task_output/gpu-capabilities-test"; then
    printf 'PASS offline GPU capability tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/ggtt_init_bounds_test.cpp -o "$task_output/ggtt-init-bounds-test" && \
    "$task_output/ggtt-init-bounds-test"; then
    printf 'PASS offline GGTT init bounds model\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/phy_translation_test.cpp -o "$task_output/phy-translation-test" && \
    "$task_output/phy-translation-test"; then
    printf 'PASS offline PHY translation tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/pattern_match_test.cpp -o "$task_output/pattern-match-test" && \
    "$task_output/pattern-match-test"; then
    printf 'PASS offline pattern matching tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/dvmt_patch_test.cpp -o "$task_output/dvmt-patch-test" && \
    "$task_output/dvmt-patch-test"; then
    printf 'PASS offline DVMT patch encoding tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/context_pool_test.cpp -o "$task_output/context-pool-test" && \
    "$task_output/context-pool-test"; then
    printf 'PASS offline context pool tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/binary_identity_test.cpp -o "$task_output/binary-identity-test" && \
    "$task_output/binary-identity-test"; then
    printf 'PASS offline binary identity tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/pci_identity_test.cpp -o "$task_output/pci-identity-test" && \
    "$task_output/pci-identity-test"; then
    printf 'PASS offline PCI identity tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/workqueue_unwind_test.cpp -o "$task_output/workqueue-unwind-test" && \
    "$task_output/workqueue-unwind-test"; then
    printf 'PASS offline workqueue unwind tests\n'
else
    failed=1
fi
exit "$failed"

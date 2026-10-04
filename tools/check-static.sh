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
for source in NootedGreen/*.cpp; do
    log="$task_output/${source##*/}.analyzer.log"
    if ! "$compiler" --target=x86_64-apple-macos13 -std=c++14 \
        --analyze -Xanalyzer -analyzer-output=text \
        -ffreestanding -fno-builtin \
        -DKERNEL=1 -DKERNEL_PRIVATE=1 -DMODULE_VERSION=100 \
        -DPRODUCT_NAME=NootedGreen -D__MAC_OS_X_VERSION_MIN_REQUIRED=130000 \
        -I. -ILilu.kext/Contents/Resources -IMacKernelSDK/Headers \
        "$source" > "$log" 2>&1; then
        printf 'FAIL analyzer: %s (see diagnostic directory)\n' "$source"
        failed=1
    elif awk '/warning:/{found=1} END{exit !found}' "$log"; then
        printf 'FAIL analyzer finding: %s (see diagnostic directory)\n' "$source"
        failed=1
    else
        printf 'PASS analyzer: %s\n' "$source"
    fi
done
# The pinned TGL bridge contains private packed ABI and MMIO structures. Keep
# regressions in format ABIs, alignment and aggregate layout as hard failures.
if ! "$compiler" --target=x86_64-apple-macos13 -std=c++14 \
    -fsyntax-only -ffreestanding -fno-builtin \
    -Werror=format -Werror=cast-align -Werror=reorder-init-list \
    -DKERNEL=1 -DKERNEL_PRIVATE=1 -DMODULE_VERSION=100 \
    -DPRODUCT_NAME=NootedGreen -D__MAC_OS_X_VERSION_MIN_REQUIRED=130000 \
    -I. -ILilu.kext/Contents/Resources -IMacKernelSDK/Headers \
    NootedGreen/kern_gen11.cpp > "$task_output/kern_gen11.strict.log" 2>&1; then
    printf 'FAIL strict Gen11 ABI warnings (see diagnostic directory)\n'
    failed=1
else
    printf 'PASS strict Gen11 format/alignment/layout warnings\n'
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/guc_ring_test.cpp -o "$task_output/guc-ring-test" && \
    "$task_output/guc-ring-test"; then
    printf 'PASS offline ring tests\n'
else
    failed=1
fi
git diff --check || failed=1
if "$compiler" -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/vf_command_pool_constructor_prototype_test.cpp \
    -o "$task_output/pool-constructor-prototype-test" && \
    "$task_output/pool-constructor-prototype-test"; then
    printf 'PASS offline pool constructor design prototype (not production)\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/vf_command_pool_growth_policy_test.cpp \
    -o "$task_output/pool-growth-policy-test" && \
    "$task_output/pool-growth-policy-test"; then
    printf 'PASS offline VF command-pool growth postcondition policy\n'
else
    failed=1
fi
if python3 -B tools/host_vf_containment_contract_test.py; then
    printf 'PASS offline host VF containment contracts\n'
else
    failed=1
fi
if python3 -B tools/host_vf_runtime_manifest_test.py; then
    printf 'PASS offline host VF runtime-manifest contracts\n'
else
    failed=1
fi
if python3 -B tools/linux_mmio_mapper_test.py; then
    printf 'PASS offline MMIO mapper/tooling contracts\n'
else
    failed=1
fi
if python3 -B tools/project_contract_test.py; then
    printf 'PASS offline Xcode/plist/scheme/CI contracts\n'
else
    failed=1
fi
if python3 -B tools/vf_command_pool_growth_source_contract_test.py \
    NootedGreen/kern_gen11.cpp; then
    printf 'PASS offline VF command-pool growth source contract\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/gpu_capabilities_test.cpp -o "$task_output/gpu-capabilities-test" && \
    "$task_output/gpu-capabilities-test"; then
    printf 'PASS offline GPU capability tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/tgl_compat_test.cpp -o "$task_output/tgl-compat-test" && \
    "$task_output/tgl-compat-test"; then
    printf 'PASS offline TGL compatibility contracts\n'
else
    failed=1
fi
if "$compiler" --target=x86_64-apple-macos13 -std=c++14 -c \
    -ffreestanding -fno-builtin -fvisibility=hidden \
    -DKERNEL=1 -DKERNEL_PRIVATE=1 -DMODULE_VERSION=100 \
    -DPRODUCT_NAME=NootedGreen -D__MAC_OS_X_VERSION_MIN_REQUIRED=130000 \
    -I. -ILilu.kext/Contents/Resources -IMacKernelSDK/Headers \
    NootedGreen/kern_tgl_compat.cpp -o "$task_output/kern-tgl-compat.o" && \
    python3 -B tools/framebuffer_compat_contract_test.py \
        "$task_output/kern-tgl-compat.o" NootedGreen/kern_tgl_compat.cpp \
        sle_Internal/le/AppleIntelTGLGraphicsFramebuffer.kext/Contents/Info.plist \
        sle_Internal/le/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer \
        sle_Internal/lep/AppleIntelTGLGraphicsFramebuffer.kext/Contents/Info.plist \
        sle_Internal/lep/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer; then
    printf 'PASS offline TGL framebuffer provider contracts\n'
else
    failed=1
fi
if python3 tools/personality_contract_test.py \
    sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/Info.plist \
    sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/Info.plist; then
    printf 'PASS offline native personality contract tests\n'
else
    failed=1
fi
if python3 -B tools/telemetry_contract_test.py \
    NootedGreen/kern_telemetry.cpp \
    sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/Info.plist \
    sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline GPU telemetry provider contract tests\n'
else
    failed=1
fi
if python3 tools/route_symbol_contract_test.py \
    NootedGreen/kern_gen11.cpp \
    sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
    sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
    sle_Internal/lep/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer \
    sle_Internal/le/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer; then
    printf 'PASS offline routed-symbol contract tests\n'
else
    failed=1
fi
if python3 -B tools/vf_timer_teardown_model_test.py; then
    printf 'PASS offline abstract VF timer teardown model\n'
else
    failed=1
fi
if python3 -B tools/vf_accelerator_lifecycle_contract_test.py \
    NootedGreen/kern_gen11.cpp \
    sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
    sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline VF accelerator lifecycle contracts\n'
else
    failed=1
fi
if python3 -B tools/vf_telemetry_isolation_contract_test.py \
    NootedGreen/kern_gen11.cpp \
    sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/Info.plist \
    sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/Info.plist \
    sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
    sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline VF telemetry/OA isolation contracts\n'
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
    tools/ggtt_rotation_test.cpp -o "$task_output/ggtt-rotation-test" && \
    "$task_output/ggtt-rotation-test"; then
    printf 'PASS offline GGTT rotation model\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_irq_gate_test.cpp -o "$task_output/vf-irq-gate-test" && \
    "$task_output/vf-irq-gate-test"; then
    printf 'PASS offline VF IRQ gate model\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_memirq_test.cpp -o "$task_output/vf-memirq-test" && \
    "$task_output/vf-memirq-test"; then
    printf 'PASS offline VF memory-IRQ protocol tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_context_shutdown_test.cpp -o "$task_output/vf-context-shutdown-test" && \
    "$task_output/vf-context-shutdown-test"; then
    printf 'PASS offline VF context shutdown model\n'
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
    tools/context_pool_test.cpp -o "$task_output/context-pool-test" && \
    "$task_output/context-pool-test"; then
    printf 'PASS offline context pool tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/binary_identity_test.cpp -o "$task_output/binary-identity-test" && \
    "$task_output/binary-identity-test" \
        sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
        sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
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
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_submission_gate_test.cpp -o "$task_output/vf-submission-gate-test" && \
    "$task_output/vf-submission-gate-test"; then
    printf 'PASS offline VF native-producer admission tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_runtime_test.cpp -o "$task_output/vf-runtime-test" && \
    "$task_output/vf-runtime-test"; then
    printf 'PASS offline VF runtime-relay/topology tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_mmio_response_test.cpp -o "$task_output/vf-mmio-response-test" && \
    "$task_output/vf-mmio-response-test"; then
    printf 'PASS offline VF MMIO-response contract tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_legacy_ctb_test.cpp -o "$task_output/vf-legacy-ctb-test" && \
    "$task_output/vf-legacy-ctb-test"; then
    printf 'PASS offline VF legacy-CTB request contract tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_runtime_patch_test.cpp -o "$task_output/vf-runtime-patch-test" && \
    "$task_output/vf-runtime-patch-test" \
        sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
        sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline VF runtime-patch anchor tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_tlb_patch_test.cpp -o "$task_output/vf-tlb-patch-test" && \
    "$task_output/vf-tlb-patch-test" \
        sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
        sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline VF physical-TLB inventory tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_standalone_patch_test.cpp -o "$task_output/vf-standalone-patch-test" && \
    "$task_output/vf-standalone-patch-test" \
        sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
        sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline VF standalone-fallback anchor tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_guc_factory_patch_test.cpp -o "$task_output/vf-guc-factory-patch-test" && \
    "$task_output/vf-guc-factory-patch-test" \
        sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
        sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline VF GuC factory cleanup anchor tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_blit3d_scratch_patch_test.cpp -o "$task_output/vf-blit3d-scratch-patch-test" && \
    "$task_output/vf-blit3d-scratch-patch-test" \
        sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
        sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline VF Blit3D scratch allocation anchor tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/unaligned_patch_test.cpp -o "$task_output/unaligned-patch-test" && \
    "$task_output/unaligned-patch-test" \
        sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics \
        sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics; then
    printf 'PASS offline bounded unaligned-store inventory tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/framebuffer_patch_test.cpp -o "$task_output/framebuffer-patch-test" && \
    "$task_output/framebuffer-patch-test" \
        sle_Internal/lep/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer \
        sle_Internal/le/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer; then
    printf 'PASS offline framebuffer bounds-patch anchor tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_guc_request_test.cpp -o "$task_output/vf-guc-request-test" && \
    "$task_output/vf-guc-request-test"; then
    printf 'PASS offline GuC FAST-request contract tests\n'
else
    failed=1
fi
if "$compiler" -std=c++14 -O1 -g -fsanitize=address,undefined \
    tools/vf_guc_event_test.cpp -o "$task_output/vf-guc-event-test" && \
    "$task_output/vf-guc-event-test"; then
    printf 'PASS offline GuC G2H-event contract tests\n'
else
    failed=1
fi
exit "$failed"

#!/usr/bin/env python3
"""Source contract for both duplicate VF event-vector growth guards."""
import sys
from pathlib import Path


REQUIRED = (
    '"__ZN15IGAccelResource22updateMappingCacheTypeEj", resourceEventOwnerStart',
    '"__GLOBAL__sub_I_IGAccelResource.cpp", resourceEventOwnerEnd',
    '"__ZN23IGAccelSharedUserClient9MetaClassD0Ev", sharedEventOwnerStart',
    '"__GLOBAL__sub_I_IGAccelSharedUserClient.cpp", sharedEventOwnerEnd',
    "constexpr const char *eventGrowSymbol =",
    '"__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb"',
    "resourceAddEvents - resourceEventGrow !=",
    "sharedAddEvents - sharedEventGrow !=",
    "reinterpret_cast<const uint8_t *>(resourceEventGrow)",
    "reinterpret_cast<const uint8_t *>(sharedEventGrow)",
    '"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",\n\t\t\t\t vfResourceEventVectorGrow',
    '"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",\n\t\t\t\t vfSharedEventVectorGrow',
    "index, resourceEventGrowRoute, 1,",
    "index, sharedEventGrowRoute, 1,",
    '"V273: guarded both VF event-vector growth instantiations"',
    "if (gVfIdentity == VfIdentity::Virtual) {\n\t\tconst size_t vectorSize =",
    "NGEventVector::hasConsistentState(",
    "NGEventVector::hasRepresentableRequest(requested)",
    'vfMarkProtocolFault("VF event-vector growth received unsafe pre-state")',
    '"V273: refusing unsafe VF event vector size=%llu capacity=%llu request=%llu"',
    "const bool nativeResult = native(vector, requested);",
    "if (gVfIdentity != VfIdentity::Virtual)",
    "NGEventVector::hasCapacity(",
    'vfMarkProtocolFault("VF event-vector growth did not publish requested capacity")',
    '"V273: refusing incomplete VF event vector size=%llu capacity=%llu request=%llu"',
    "reinterpret_cast<Grow>(callback->oVfResourceEventVectorGrow)",
    "reinterpret_cast<Grow>(callback->oVfSharedEventVectorGrow)",
)


def accepts(source: str) -> bool:
    if any(fragment not in source for fragment in REQUIRED):
        return False
    begin = source.index("mach_vm_address_t resourceEventOwnerStart")
    end = source.index("KernelPatcher::RouteRequest workQueueInitRoute", begin)
    install = source[begin:end]
    if not (install.index("eventOwnerBounds") <
            install.index("resourceEventHelpers") <
            install.index("sharedEventHelpers") <
            install.index("Changed VF event-vector growth contracts") <
            install.index("resourceEventGrowRoute") <
            install.index("sharedEventGrowRoute") <
            install.index("index, resourceEventGrowRoute, 1,") <
            install.index("index, sharedEventGrowRoute, 1,")):
        return False
    helper_begin = source.index("static bool vfEventVectorGrowChecked(")
    helper_end = source.index("bool Gen11::vfResourceEventVectorGrow", helper_begin)
    helper = source[helper_begin:helper_end]
    return (helper.index("hasConsistentState(") <
            helper.index("VF event-vector growth received unsafe pre-state") <
            helper.index("refusing unsafe VF event vector") <
            helper.index("native(vector, requested)") <
            helper.index("gVfIdentity != VfIdentity::Virtual") <
            helper.index("hasCapacity(") <
            helper.index("VF event-vector growth did not publish requested capacity") <
            helper.index("refusing incomplete VF event vector"))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: vf_event_vector_source_contract_test.py kern_gen11.cpp")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    assert accepts(source), "missing or reordered VF event-vector contract"
    for fragment in REQUIRED:
        assert source.count(fragment) == 1, f"ambiguous source contract: {fragment}"
        assert not accepts(source.replace(fragment, "MUTATED", 1)), \
            f"mutation escaped source contract: {fragment}"
    print(f"PASS: dual VF event-vector routes and {len(REQUIRED)} rejected mutations")


if __name__ == "__main__":
    main()

#include "../NootedGreen/kern_vf_context_shutdown.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../NootedGreen/kern_vf_device_observation.hpp"

using NGVfContextShutdown::Action;

int main() {
    {
        using NGVfTimeoutObservation::Reference;
        Reference reference{};
        assert(!reference.matches(7,0x100319,0x20000020,1));
        reference={7,0x100319,0x20000020,99,true};
        assert(reference.matches(7,0x100319,0x20000020,99));
        assert(!reference.matches(8,0x100319,0x20000020,99));
        assert(!reference.matches(7,0x101319,0x20000020,99));
        assert(!reference.matches(7,0x100319,0x60000020,99));
        assert(!reference.matches(7,0x100319,0x20000020,100));
        for(unsigned bit=0;bit<32;++bit) {
            assert(!reference.matches(7^(1U<<bit),0x100319,0x20000020,99));
            assert(!reference.matches(7,0x100319^(1U<<bit),0x20000020,99));
            assert(!reference.matches(7,0x100319,0x20000020^(1U<<bit),99));
        }
        for(unsigned bit=0;bit<64;++bit)
            assert(!reference.matches(7,0x100319,0x20000020,99^(uint64_t(1)<<bit)));
        reference.valid=false;
        assert(!reference.matches(7,0x100319,0x20000020,99));
        reference={7,0x100319,0x20000020,0,true};
        assert(!reference.matches(7,0x100319,0x20000020,0));
        // Six fixed frames (id/flags/state/expectedStamp/observedStamp/
        // expectedHead/reportedHead/LRCAhead/LRCAtail), no pointer payload.
        char message[1024]{};
        size_t used=std::snprintf(message,sizeof(message),
            "V267: refusing VF event timeout recovery channel=%d V364: capture=%x",INT32_MIN,UINT32_MAX);
        for(unsigned engine=0;engine<NGVfTimeoutObservation::engineCount;++engine) {
            int count=std::snprintf(message+used,sizeof(message)-used,
                " T%u=%x/%x/%x/%x/%x/%x/%x/%x/%x",engine,
                UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,
                UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX);
            assert(count>0 && static_cast<size_t>(count)<sizeof(message)-used);
            used+=static_cast<size_t>(count);
        }
        uint32_t words[48]{};char encoded[257]{};
        for(unsigned i=0;i<48;++i)words[i]=0x03020100U+i*0x04040404U;
        NGVfTimeoutObservation::encodeRingPrefix(words,encoded);
        assert(std::strlen(encoded)==256 && std::memcmp(encoded,"AAECAwQF",8)==0);
        constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for(unsigned i=0;i<64;++i) {
            uint32_t value=0;
            for(unsigned j=0;j<4;++j) {
                const char* at=std::strchr(alphabet,encoded[i*4+j]);assert(at && *at);
                value=(value<<6)|uint32_t(at-alphabet);
            }
            for(unsigned j=0;j<3;++j)assert(((value>>((2-j)*8))&255U)==i*3+j);
        }
        int extra=std::snprintf(message+used,sizeof(message)-used," V366: P=%x/%x/%.256s",UINT32_MAX,UINT32_MAX,encoded);
        assert(extra>0 && size_t(extra)<sizeof(message)-used);used+=size_t(extra);
        assert(used<950 && std::strlen(message)==used && std::strstr(message," T5=") && std::strstr(message," V366: P="));
        for(uint32_t bytes=256;bytes<=0x200000;bytes*=2)
            for(uint32_t tail=32;tail<=192;tail+=8)
                assert(NGVfTimeoutObservation::ringPrefixAdmitted(bytes,bytes,tail,0));
        for(auto bad: {0U,1U,32U,64U,192U,255U,257U,0x400000U})
            assert(!NGVfTimeoutObservation::ringPrefixAdmitted(bad,UINT64_MAX,32,0));
        for(auto bad: {0U,1U,24U,31U,33U,60U,193U,200U,256U,UINT32_MAX})
            assert(!NGVfTimeoutObservation::ringPrefixAdmitted(256,256,bad,0));
        assert(!NGVfTimeoutObservation::ringPrefixAdmitted(256,255,184,0));
        assert(!NGVfTimeoutObservation::ringPrefixAdmitted(256,256,184,4));
    }
    {
        NGVfTaskCompletion::Request request{NGVfTaskCompletion::version,sizeof(NGVfTaskCompletion::Request),{}};
        for(auto& event:request.expected)event=UINT64_MAX;
        assert(!NGVfTaskCompletion::validRequest(request));
        request.expected[0]=(uint64_t(7)<<32)|1;
        assert(NGVfTaskCompletion::validRequest(request));
        request.expected[1]=request.expected[0];
        assert(!NGVfTaskCompletion::validRequest(request));request.expected[1]=UINT64_MAX;
        request.expected[0]=(uint64_t(7)<<32)|8;
        assert(!NGVfTaskCompletion::validRequest(request));request.expected[0]=1;
        request.bytes=71;assert(!NGVfTaskCompletion::validRequest(request));request.bytes=72;
        request.abiVersion=2;assert(!NGVfTaskCompletion::validRequest(request));request.abiVersion=1;
        struct Args {
            uint32_t version=0,selector=NGVfTaskCompletion::selector;
            void *asyncWakePort=nullptr,*asyncReference=nullptr;uint32_t asyncReferenceCount=0;
            void *scalarInput=nullptr,*scalarOutput=nullptr;uint32_t scalarInputCount=0,scalarOutputCount=0;
            const void* structureInput=nullptr;size_t structureInputSize=0;void* structureInputDescriptor=nullptr;
            void* structureOutput=nullptr;size_t structureOutputSize=0;
            void* structureOutputDescriptor=nullptr;size_t structureOutputDescriptorSize=0;
            void* structureVariableOutputData=nullptr;
        } args;
        NGVfTaskCompletion::Reply reply{};
        args.structureInput=&request;args.structureInputSize=sizeof(request);
        args.structureOutput=&reply;args.structureOutputSize=sizeof(reply);
        auto shape=[&]() {return NGVfTaskCompletion::inlineShape(&args,0,nullptr,nullptr,nullptr);};
        assert(shape());args.structureInputDescriptor=&request;assert(!shape());args.structureInputDescriptor=nullptr;
        args.structureOutputSize--;assert(!shape());args.structureOutputSize++;
        args.scalarInputCount=1;assert(!shape());args.scalarInputCount=0;
        // Native MIG retains buffer addresses with zero scalar counts.
        args.scalarInput=&request;args.scalarOutput=&reply;assert(shape());
        args.scalarOutputCount=1;assert(!shape());args.scalarOutputCount=0;
        assert(!NGVfTaskCompletion::inlineShape(&args,0,&request,nullptr,nullptr));
        args.selector=NGVfDeviceObservation::selector;assert(!shape());
        args.structureInputSize=0; // MIG inband address may remain non-NULL.
        args.structureOutputSize=sizeof(NGVfDeviceObservation::Reply);
        assert(NGVfDeviceObservation::inlineShape(&args,0,nullptr,nullptr,nullptr));
        args.selector=NGVfAvcPublication::selector;
        args.structureOutputSize=sizeof(NGVfAvcPublication::Reply);
        auto avcShape=[&]() {return NGVfAvcPublication::inlineShape(&args,0,nullptr,nullptr,nullptr);};
        assert(avcShape());
        args.scalarInputCount=1;assert(!avcShape());args.scalarInputCount=0;
        args.scalarOutputCount=1;assert(!avcShape());args.scalarOutputCount=0;
        args.structureInputSize=1;assert(!avcShape());args.structureInputSize=0;
        args.structureInputDescriptor=&request;assert(!avcShape());args.structureInputDescriptor=nullptr;
        args.structureOutputDescriptor=&reply;assert(!avcShape());args.structureOutputDescriptor=nullptr;
        args.structureOutputDescriptorSize=1;assert(!avcShape());args.structureOutputDescriptorSize=0;
        args.structureVariableOutputData=&reply;assert(!avcShape());args.structureVariableOutputData=nullptr;
        args.structureOutputSize--;assert(!avcShape());args.structureOutputSize++;
        assert(!NGVfAvcPublication::inlineShape(&args,0,&request,nullptr,nullptr));
        assert(avcShape());
        reply.abiVersion=1;reply.bytes=sizeof(reply);reply.registryId=42;reply.recordCount=1;
        for(unsigned i=0;i<16;++i)reply.familyUuid[i]=NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid[i];
        reply.records[0]={1,3,7,0,4};
        assert(NGVfTaskCompletion::validReply(reply,42,request));
        auto malformed=reply;malformed.records[0].publishedSerial=0;
        assert(!NGVfTaskCompletion::validReply(malformed,42,request));
        malformed=reply;malformed.records[0].stamp=1;
        assert(!NGVfTaskCompletion::validReply(malformed,42,request));
        malformed=reply;malformed.records[1].head=4;
        assert(!NGVfTaskCompletion::validReply(malformed,42,request));
        malformed=reply;malformed.familyUuid[0]^=1;
        assert(!NGVfTaskCompletion::validReply(malformed,42,request));
        assert(!NGVfTaskCompletion::validReply(reply,43,request));
        NGVfTaskCompletion::Reply output{};output.registryId=999;
        auto call=[&](uint32_t selector,const auto* input,size_t inputBytes,void* out,size_t* bytes) noexcept {
            assert(selector==NGVfTaskCompletion::selector && input==&request && inputBytes==sizeof(request));
            assert(*bytes==sizeof(reply));*static_cast<NGVfTaskCompletion::Reply*>(out)=reply;return 0;
        };
        assert(NGVfTaskCompletion::query(42,request,call,output) && output.registryId==42);
        output.registryId=999;reply.reserved=1;
        assert(!NGVfTaskCompletion::query(42,request,call,output) && output.registryId==999);
    }
    // Metadata coverage is not GPU completion. Enumerate accepted/rejected
    // transactions and interfering unknown writers against a simple oracle.
    for (unsigned marker = 0; marker < 2; ++marker)
        for (unsigned accept = 0; accept < 2; ++accept)
            for (unsigned interfere = 0; interfere < 2; ++interfere)
                for (uint32_t stamp : {0U, 1U, 0x7fffffffU, 0x80000000U, UINT32_MAX}) {
                    NGVfSubmissionCoverage::Tracker tracker {};
                    tracker.covered = true; // prior submitted work had a marker
                    const uint64_t token = tracker.begin(7, stamp, 64, 60, marker);
                    assert(token && !tracker.hasMarkerCoverage());
                    assert(!tracker.begin(8, stamp, 64, 60, true));
                    assert(!tracker.claim(token, 8, stamp, 64));
                    assert(!tracker.claim(token, 7, stamp ^ 1, 64));
                    assert(!tracker.claim(token, 7, stamp, 72));
                    assert(!tracker.publish(token, 7, stamp, 64));
                    assert(tracker.claim(token, 7, stamp, 64));
                    assert(!tracker.claim(token, 7, stamp, 64));
                    if (interfere)
                        tracker.invalidate();
                    if (accept) {
                        assert(tracker.publish(token, 7, stamp, 64));
                        assert(!tracker.publish(token, 7, stamp, 64));
                    }
                    assert(!tracker.finish(token, 8, accept));
                    assert(tracker.finish(token, 7, accept));
                    const bool oracle = !interfere && (!accept || marker);
                    assert(tracker.hasMarkerCoverage() == oracle);
                    if (accept)
                        assert(tracker.coveredStamp == stamp && tracker.coveredTail == 64 &&
                               tracker.coveredHead == 60 && tracker.hasSubmittedWork());
                    assert(!tracker.finish(token, 7, accept));
                    const uint64_t next = tracker.begin(7, stamp, 72, 72, true);
                    assert(next != token && !tracker.claim(token, 7, stamp, 72));
                    assert(tracker.finish(next, 7, false));
                }
    for (unsigned published = 0; published < 2; ++published) {
        NGVfSubmissionCoverage::Tracker tracker {};
        const auto token = tracker.begin(1, 2, 8, 4, true);
        assert(tracker.claim(token, 1, 2, 8));
        if (published)
            assert(tracker.publish(token, 1, 2, 8));
        assert(!tracker.finish(token, 1, !published));
        assert(!tracker.hasMarkerCoverage());
    }
    NGVfSubmissionCoverage::Tracker exhausted {};
    exhausted.serial = UINT64_MAX;
    assert(!exhausted.begin(1, 0, 8, 4, true));
    assert(!exhausted.begin(0, 0, 8, 4, true));
    NGVfSubmissionCoverage::Tracker lateWriter {};
    const auto lateToken = lateWriter.begin(1, 2, 8, 4, true);
    assert(lateWriter.claim(lateToken, 1, 2, 8));
    assert(lateWriter.publish(lateToken, 1, 2, 8));
    lateWriter.invalidate();
    assert(lateWriter.finish(lateToken, 1, true));
    assert(!lateWriter.hasMarkerCoverage());
    NGVfSubmissionCoverage::Tracker reused {};
    for (uint64_t generation = 1; generation <= 1024; ++generation) {
        const auto token = reused.begin(7, 5, 8, 4, true);
        assert(token == generation);
        // An active slot cannot be reset; taint survives until its finish.
        assert(!reused.resetForReuse());
        assert(reused.claim(token, 7, 5, 8));
        assert(reused.publish(token, 7, 5, 8));
        assert(reused.finish(token, 7, true));
        assert(!reused.hasMarkerCoverage());
        assert(reused.resetForReuse());
        assert(reused.serial == generation);
        assert(!reused.claim(token, 7, 5, 8));
    }
    const auto newest = reused.begin(7, 5, 8, 4, true);
    for (uint64_t stale = 1; stale < newest; ++stale) {
        assert(!reused.claim(stale, 7, 5, 8));
        assert(!reused.publish(stale, 7, 5, 8));
        assert(!reused.finish(stale, 7, true));
    }
    assert(reused.claim(newest, 7, 5, 8));
    assert(reused.publish(newest, 7, 5, 8));
    assert(reused.finish(newest, 7, true));
    assert(reused.hasMarkerCoverage());
    assert(reused.resetForReuse() && !reused.hasMarkerCoverage());
    assert(!reused.hasSubmittedWork());
    exhausted.covered = true;
    assert(exhausted.resetForReuse());
    assert(exhausted.serial == UINT64_MAX && !exhausted.hasMarkerCoverage());
    assert(!exhausted.begin(1, 5, 8, 4, true));

    // The final native ring transaction is either [MI_REPORT_HEAD] at the
    // last DWord or [MI_REPORT_HEAD, MI_NOOP] when padding is required. Check
    // wraparound and reject every malformed geometry/final-word pairing.
    for (uint32_t ringSize : {16U, 4096U, 65536U, 0x200000U}) {
        for (uint32_t tail : {0U, 8U, ringSize - 8U}) {
            uint32_t head = UINT32_MAX;
            assert(NGVfSubmissionCoverage::reportHeadForTail(
                tail, ringSize, NGVfSubmissionCoverage::miReportHead,
                0xA5A5A5A5U, head));
            assert(head == tail);
            assert(NGVfSubmissionCoverage::reportHeadForTail(
                tail, ringSize, 0, NGVfSubmissionCoverage::miReportHead,
                head));
            assert(head == tail);
            assert(!NGVfSubmissionCoverage::reportHeadForTail(
                tail, ringSize, 0, 0, head));
        }
    }
    for (uint32_t invalidSize : {0U, 8U, 24U, 0x400000U}) {
        uint32_t head = 0;
        assert(!NGVfSubmissionCoverage::reportHeadForTail(
            0, invalidSize, NGVfSubmissionCoverage::miReportHead, 0, head));
    }
    for (uint32_t invalidTail : {4U, 4096U, UINT32_MAX}) {
        uint32_t head = 0;
        assert(!NGVfSubmissionCoverage::reportHeadForTail(
            invalidTail, 4096, NGVfSubmissionCoverage::miReportHead, 0, head));
    }

    // A software-observed stamp is accepted only with exact marker coverage,
    // Tahoe's signed wrap-safe ordering and the independent reported head.
    for (uint32_t requested : {0U, 1U, 0x7FFFFFFFU, 0x80000000U,
                               0xFFFFFFFEU, UINT32_MAX}) {
        NGVfSubmissionCoverage::Tracker tracker {};
        const auto token = tracker.begin(9, requested, 64, 60, true);
        assert(tracker.claim(token, 9, requested, 64));
        assert(tracker.publish(token, 9, requested, 64));
        assert(tracker.finish(token, 9, true));
        for (uint32_t observed : {0U, 1U, 0x7FFFFFFFU, 0x80000000U,
                                  0xFFFFFFFEU, UINT32_MAX}) {
            const bool stampReached =
                static_cast<int32_t>(requested - observed) <= 0;
            for (bool softwareCompletionPossible : {false, true}) {
                assert(tracker.gpuComplete(
                    observed, 0xABC00000U | 60U,
                    softwareCompletionPossible) ==
                    (stampReached && !softwareCompletionPossible));
                assert(!tracker.gpuComplete(
                    observed, 56, softwareCompletionPossible));
            }
        }
    }
    NGVfSubmissionCoverage::Tracker unmarked {};
    const auto unmarkedToken = unmarked.begin(1, 7, 8, 4, false);
    assert(unmarked.claim(unmarkedToken, 1, 7, 8));
    assert(unmarked.publish(unmarkedToken, 1, 7, 8));
    assert(unmarked.finish(unmarkedToken, 1, true));
    assert(unmarked.hasSubmittedWork() && !unmarked.hasMarkerCoverage());
    assert(!unmarked.gpuComplete(7, 4, false));
    {
        NGVfSubmissionCoverage::Tracker tracker{};
        NGVfSubmissionCoverage::Tracker::CompletionEvidence evidence{99,88,77};
        assert(!tracker.completionEvidence(1,7,4,false,evidence));
        assert(evidence.observationSerial==99);
        const auto token=tracker.begin(1,7,8,4,true);
        assert(tracker.claim(token,1,7,8) && tracker.publish(token,1,7,8));
        assert(!tracker.completionEvidence(token,7,4,false,evidence)); // active owner
        assert(tracker.finish(token,1,true));
        assert(!tracker.completionEvidence(token+1,7,4,false,evidence));
        assert(!tracker.completionEvidence(token,6,4,false,evidence));
        assert(!tracker.completionEvidence(token,7,8,false,evidence));
        assert(!tracker.completionEvidence(token,7,4,true,evidence));
        assert(evidence.observationSerial==99);
        assert(tracker.completionEvidence(token,7,4,false,evidence));
        assert(evidence.observationSerial==token && evidence.stamp==7 && evidence.head==4);
        const auto rejected=tracker.begin(1,9,16,12,true);
        assert(rejected!=token && tracker.finish(rejected,1,false));
        assert(tracker.gpuComplete(7,4,false)); // Old idle policy deliberately unchanged.
        assert(!tracker.completionEvidence(rejected,7,4,false,evidence));
        assert(!tracker.completionEvidence(token,7,4,false,evidence));
        assert(tracker.resetForReuse());
        assert(!tracker.coveredSerial);
        assert(!tracker.completionEvidence(token,7,4,false,evidence));
        assert(!unmarked.completionEvidence(unmarkedToken,7,4,false,evidence));
    }

    // Independent contexts may reuse the same numeric token. The production
    // bridge must first select/pin the exact context; a token is not a global
    // identity. Within that context, wrong owners and stale invocations must
    // not consume the live claim or finish, even after CTB publication.
    for (unsigned marker = 0; marker < 2; ++marker)
        for (unsigned prior = 0; prior < 2; ++prior)
            for (unsigned invalidationPhase = 0; invalidationPhase < 4; ++invalidationPhase) {
                NGVfSubmissionCoverage::Tracker tracker {};
                tracker.covered = prior;
                const auto token = tracker.begin(7, 5, 8, 4, marker);
                const auto rejectForeign = [&]() {
                    const auto before = tracker;
                    for (uint64_t wrongOwner : {0ULL, 8ULL}) {
                        assert(!tracker.claim(token, wrongOwner, 5, 8));
                        assert(!tracker.publish(token, wrongOwner, 5, 8));
                        assert(!tracker.finish(token, wrongOwner, true));
                        assert(!tracker.finish(token, wrongOwner, false));
                    }
                    for (uint64_t stale : std::array<uint64_t, 3>{{0, 2, UINT64_MAX}}) {
                        assert(!tracker.claim(stale, 7, 5, 8));
                        assert(!tracker.publish(stale, 7, 5, 8));
                        assert(!tracker.finish(stale, 7, true));
                    }
                    assert(tracker.serial == before.serial && tracker.owner == before.owner);
                    assert(tracker.stamp == before.stamp && tracker.tail == before.tail &&
                           tracker.reportHead == before.reportHead);
                    assert(tracker.claimed == before.claimed && tracker.published == before.published);
                    assert(tracker.carriesStamp == before.carriesStamp && tracker.tainted == before.tainted);
                    assert(tracker.covered == before.covered);
                    assert(tracker.coveredStamp == before.coveredStamp &&
                           tracker.coveredTail == before.coveredTail &&
                           tracker.coveredHead == before.coveredHead &&
                           tracker.submitted == before.submitted);
                };
                rejectForeign();
                if (invalidationPhase == 1)
                    tracker.invalidate();
                assert(tracker.claim(token, 7, 5, 8));
                rejectForeign();
                if (invalidationPhase == 2)
                    tracker.invalidate();
                assert(tracker.publish(token, 7, 5, 8));
                rejectForeign();
                if (invalidationPhase == 3)
                    tracker.invalidate();
                assert(!tracker.hasMarkerCoverage());
                assert(tracker.finish(token, 7, true));
                assert(tracker.hasMarkerCoverage() == (marker && invalidationPhase == 0));
            }

    // Independent widened endpoint oracle for all nearby slot/buffer edges.
    for (int32_t index = -2; index <= 192; ++index) {
        for (uint64_t bytes = 0; bytes <= 0x3040; ++bytes) {
            for (uint64_t scratch : {0ULL, 7ULL, 8ULL, 4096ULL}) {
                const bool expectedBacking = index >= 0 && scratch >= 8 &&
                    (static_cast<uint64_t>(index) + 1) * 64 <= bytes;
                assert(NGVfContextShutdown::validPacketBacking(index, bytes, scratch) ==
                       expectedBacking);
            }
        }
    }
    assert(!NGVfContextShutdown::validPacketBacking(-1, UINT64_MAX, UINT64_MAX));
    assert(NGVfContextShutdown::validPacketBacking(INT32_MAX, UINT64_MAX, 8));
    const uint64_t lastEndpoint = (static_cast<uint64_t>(INT32_MAX) + 1) * 64;
    assert(!NGVfContextShutdown::validPacketBacking(INT32_MAX, lastEndpoint - 1, 8));
    assert(NGVfContextShutdown::validPacketBacking(INT32_MAX, lastEndpoint, 8));

    const std::array<Action, 9> expected {{
        Action::Complete,   // Empty
        Action::Complete,   // Tombstone
        Action::Wait,       // Registering
        Action::Deregister, // Registered
        Action::Wait,       // PendingEnable
        Action::Disable,    // Enabled
        Action::Wait,       // PendingDisable
        Action::Deregister, // Disabled
        Action::Wait,       // PendingDeregister
    }};

    for (unsigned state = 0; state < expected.size(); ++state) {
        assert(NGVfContextShutdown::action(
                   static_cast<VfGucContextState>(state)) == expected[state]);
    }
    // The fixed uint8_t representation also admits unknown state bytes.
    // None may authorize shutdown completion or consume a pending event.
    for (unsigned rawState = expected.size(); rawState <= UINT8_MAX; ++rawState) {
        const auto unknown = static_cast<VfGucContextState>(rawState);
        assert(NGVfContextShutdown::action(unknown) == Action::Wait);
        for (unsigned enable = 0; enable <= 1; ++enable)
            for (unsigned disable = 0; disable <= 1; ++disable)
                for (uint32_t runnable : {0U, 1U, 2U, UINT32_MAX}) {
                    const auto result = NGVfContextEvent::scheduleDone(
                        unknown, enable != 0, disable != 0, runnable);
                    assert(!result.handled && result.state == unknown);
                    assert(result.enablePending == (enable != 0));
                    assert(result.disablePending == (disable != 0));
                }
    }

    // Exhaust the failed-attach compensation boundary. Once registration was
    // published, neither a failed send nor an unobserved DEREGISTER_DONE can be
    // reported as a safely retired context.
    for (unsigned registered = 0; registered <= 1; ++registered) {
        for (unsigned sent = 0; sent <= 1; ++sent) {
            for (unsigned tombstone = 0; tombstone <= 1; ++tombstone) {
                const bool complete =
                    NGVfContextShutdown::registrationCleanupComplete(
                        registered != 0, sent != 0, tombstone != 0);
                assert(complete ==
                       (!registered || (sent && tombstone)));
            }
        }
    }

    // Exhaust every lifecycle state, pending-token combination, and the two
    // defined runnable payloads plus malformed values. Enable completions have
    // ordering priority when both tokens exist; a disable cannot overtake one.
    for (unsigned rawState = 0; rawState < expected.size(); ++rawState) {
        for (unsigned enable = 0; enable <= 1; ++enable) {
            for (unsigned disable = 0; disable <= 1; ++disable) {
                for (uint32_t runnable : {0U, 1U, 2U, 0x80000000U, UINT32_MAX}) {
                    const auto state =
                        static_cast<VfGucContextState>(rawState);
                    const auto result = NGVfContextEvent::scheduleDone(
                        state, enable != 0, disable != 0, runnable);
                    const bool enableState =
                        state == kVfGucContextPendingEnable ||
                        state == kVfGucContextEnabled ||
                        state == kVfGucContextPendingDisable;
                    const bool enableDone = enable && runnable == 1U &&
                                            enableState;
                    const bool disableDone = !enable && disable &&
                                             runnable == 0U &&
                                             state == kVfGucContextPendingDisable;
                    assert(result.handled == (enableDone || disableDone));
                    assert(result.enablePending ==
                           ((enable != 0) && !enableDone));
                    assert(result.disablePending ==
                           ((disable != 0) && !disableDone));

                    auto wantedState = state;
                    if (enableDone && state == kVfGucContextPendingEnable)
                        wantedState = kVfGucContextEnabled;
                    else if (disableDone)
                        wantedState = kVfGucContextDisabled;
                    assert(result.state == wantedState);
                }
            }
        }
    }

    // Model the only active shutdown chain: Enabled -> PendingDisable ->
    // Disabled -> PendingDeregister -> Tombstone. Pending states are completed
    // only by their matching GuC event and are never skipped by the sweeper.
    VfGucContextState state = kVfGucContextEnabled;
    assert(NGVfContextShutdown::action(state) == Action::Disable);
    state = kVfGucContextPendingDisable;
    assert(NGVfContextShutdown::action(state) == Action::Wait);
    state = kVfGucContextDisabled;
    assert(NGVfContextShutdown::action(state) == Action::Deregister);
    state = kVfGucContextPendingDeregister;
    assert(NGVfContextShutdown::action(state) == Action::Wait);
    state = kVfGucContextTombstone;
    assert(NGVfContextShutdown::action(state) == Action::Complete);

    struct Context {
        uint32_t lrcaPage;
        uint32_t descriptorLo;
        uint32_t descriptorHi;
        uint16_t refCount;
        uint8_t engineClass;
        uint8_t engineInstance;
        VfGucContextState state;
        bool enablePending;
        bool disablePending;
        void *task;
        void *contextBacking;
        void *ringBacking;
        void *stampBacking;
        void *scratchBacking;
    };
    int backing = 0;
    int ringBacking = 0;
    int stampBacking = 0;
    int scratchBacking = 0;
    int task = 0;
    for (unsigned rawState = 0; rawState <= UINT8_MAX; ++rawState)
    for (unsigned enable = 0; enable <= 1; ++enable)
    for (unsigned disable = 0; disable <= 1; ++disable) {
        const auto initialState = static_cast<VfGucContextState>(rawState);
        Context context {0x12345000U, 0x12345309U, 0xA5A20020U, 7,
                         4, 2, initialState, enable != 0, disable != 0, &task,
                         &backing, &ringBacking,
                         &stampBacking, &scratchBacking};
        const bool handled = NGVfContextEvent::deregisterDone(context);
        assert(handled ==
               (initialState == kVfGucContextPendingDeregister));
        assert(context.state == (handled ? kVfGucContextTombstone :
                                           initialState));
        // Firmware completion must not erase identity needed by late detach.
        assert(context.lrcaPage == 0x12345000U);
        assert(context.descriptorLo == 0x12345309U);
        assert(context.descriptorHi == 0xA5A20020U);
        assert(context.engineClass == 4);
        assert(context.engineInstance == 2);
        assert(context.task == &task);
        assert(context.contextBacking == &backing);
        assert(context.ringBacking == &ringBacking);
        assert(context.stampBacking == &stampBacking);
        assert(context.scratchBacking == &scratchBacking);
        assert(context.refCount == 7);
        assert(context.enablePending == ((enable != 0) && !handled));
        assert(context.disablePending == ((disable != 0) && !handled));
    }

    // Exact retained V374 CPU ring: initial sequence followed by one BBS.
    // This checks decode only, not GPU execution or valid leaf mappings.
    uint32_t prefix[] = {
        0x13044003,0x40000000,0,0,0,0x11000001,0x1c0244,0x80000,
        0x0e009002,0,0x400010c0,0,0x11000001,0x1c0244,0x80008,
        0x13004003,0x400020c0,0,2,0,0x0e009002,2,0x40001000,0,
        0x03800000,0,0x11000001,0x1c0244,0x80000,0x0e009002,0,
        0x400010c0,0,0x11000001,0x1c0244,0x80008,
        0x18800101,0x40010000,0,0,0x13004103,0x400010c0,0,1,0,0x03800000
    };
    using NGVfTimeoutObservation::cpuRingBatchTargets;
    auto targets = cpuRingBatchTargets(prefix, sizeof(prefix)/sizeof(prefix[0]));
    assert(targets.complete && targets.count == 1 &&
           targets.addresses[0] == 0x40010000 && targets.offsets[0] == 0x90);
    const uint32_t shortPrefix[] = {
        0x13044003,0x40000000,0,0,0,0x11000001,0x1c0244,0x80000,
        0x0e009002,0,0x400010c0,0,0x11000001,0x1c0244,0x80008,
        0x18800101,0x40010000,0,0,0x13004103,0x400010c0,0,1,0,0x03800000,0
    };
    targets = cpuRingBatchTargets(shortPrefix, 26);
    assert(targets.complete && targets.count == 1 && targets.offsets[0] == 0x3c);
    assert(!cpuRingBatchTargets(nullptr, 1).complete);
    assert(!cpuRingBatchTargets(prefix, 49).complete);
    assert(!cpuRingBatchTargets(prefix, 38).complete); // truncated BBS
    prefix[38] = 0x8000; // noncanonical 48-bit address
    assert(!cpuRingBatchTargets(prefix, 46).complete);
    prefix[38] = 0;
    prefix[37] |= 1;
    assert(!cpuRingBatchTargets(prefix, 46).complete);
    prefix[37] &= ~1U;
    prefix[36] = 0x18800001; // GGTT BBS is not a PPGTT target
    assert(!cpuRingBatchTargets(prefix, 46).complete);
    prefix[36] = 0x18800101;
    prefix[45] = 0xdeadbeef;
    targets = cpuRingBatchTargets(prefix, 46);
    assert(!targets.complete && targets.count == 0); // no partial successes
    const uint32_t many[] = {0x18800101,0x1000,0,0x18800101,0x2000,0,
                             0x18800101,0x3000,0};
    assert(cpuRingBatchTargets(many, 6).count == 2);
    assert(!cpuRingBatchTargets(many, 9).complete);
    const uint32_t zeroTarget[] = {0x18800101,0,0};
    assert(!cpuRingBatchTargets(zeroTarget, 3).complete);
    const uint32_t upperCanonical[] = {0x18800101,0x1000,0xffff8000};
    assert(cpuRingBatchTargets(upperCanonical, 3).complete);
    // A BBS-looking immediate inside a known five-word packet is not parsed.
    const uint32_t immediate[] = {0x13000003,0x18800101,0x1000,0,0};
    targets = cpuRingBatchTargets(immediate, 5);
    assert(targets.complete && targets.count == 0);

    Context released {0x12345000U, 0x12345309U, 0xA5A20020U, 0,
                      4, 2, kVfGucContextTombstone, false, false, &task,
                      &backing, &ringBacking,
                      &stampBacking, &scratchBacking};
    NGVfContextEvent::clearReleasedIdentity(released);
    assert(released.lrcaPage == 0 && released.descriptorLo == 0 &&
           released.descriptorHi == 0 && released.engineClass == 0 &&
           released.engineInstance == 0 && released.task == nullptr &&
           released.contextBacking == nullptr &&
           released.ringBacking == nullptr && released.stampBacking == nullptr &&
           released.scratchBacking == nullptr);
    assert(released.refCount == 0 &&
           released.state == kVfGucContextTombstone);
}

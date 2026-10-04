#!/usr/bin/env python3
"""Prove that the VF engine wrapper preserves Apple's IOAccel lifecycle tail."""

import pathlib
import hashlib
import struct
import sys

# Complete reviewed native bodies. This fixes the concrete Intel override
# graph, not inherited timer APIs, dynamic callbacks or runtime completion.
STAMP_IRQ_NATIVE = {
    "__ZN15IGAccelResource31createAndPrepareRotationMappingEv": (0xf4, "3ecec4f2acb2a437f81f304a571663c062632f47270ba1a17993e9a254029e3b"),
    "__ZN16IGAccelMemoryMap4initEP22IOGraphicsAccelerator2P11IOAccelTaskP13IOAccelMemoryj": (0x68, "970afaf15c9854e913ad4aa67af32f4ff61ca1267ba3f4f06ad6dd1f15fea892"),
    "__ZN31IGHardwarePerProcessPageTable6415mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory": (0x2cc, "3b6131194a023eb7513ccbb26a210c5fa2288c4fc74d66723188a46887a1b11b"),
    "__ZN15IGMemoryManager16initDeviceMemoryEv": (0x410, "bd63dc4bc1ab41493a9bd375699d29d377a83561aa5f85a59d167a5eb4bfa82d"),
    "__ZN15IGMemoryManager12initSegmentsEv": (0xb4, "b9cf1738915b0a609c085aec9f83a6f06322d87f24ea16c51918e46063915cad"),
    "__ZN31IGHardwarePerProcessPageTable3210unmapRangeERK14IGAddressRange": (0x8a, "62cbf1c6629858563f2c06351cf8643f3c1541773f4e3b1fc038027efda7a586"),
    "__ZN31IGHardwarePerProcessPageTable3231getPageTableRootPhysicalAddressEPy": (0x20, "cd25ae2ea8652e97f3c1c2886a2e0bc2d3f768108abdf7f275e8469773dbb4e2"),
    "__ZN31IGHardwarePerProcessPageTable6431getPageTableRootPhysicalAddressEPy": (0x20, "93122c3d3b36d35eb68786330a138b026db29a9bd24d533287dd739a8060643d"),
    "__ZN31IGHardwarePerProcessPageTable326expandE19GTTVirtualAddress32": (0x23e, "8b0e2172003ce89b7d08c603f55535e1c54d2c3d9380e53da00ad5ff83cde343"),
    "__ZN23IGAccelSharedUserClient11sharedStartEv": (0x40, "3ad72464337cc4e9fa4ac10461f44c34256cde6d1635212fc5cc34fe0fe510d1"),
    "__ZN11IGAccelTask11withOptionsEP16IntelAccelerator": (0x48, "294990cf7ca14e27020ecc569064444b73504a32e841c0856acbdb568081f207"),
    "__ZN16IntelAccelerator19createKernelGPUTaskEv": (0xa, "30021915389fd196a6e879b21a175f135be03e009e385a325f9fc33595937ba6"),
    "__ZN16IntelAccelerator17createUserGPUTaskEv": (0x3c, "3ff9c8b607763de74cd8eccb7125f9a4abb7261a59fa48f2ef78dafeabbef377"),
    "__ZN11IGAccelTask15initWithOptionsEP16IntelAccelerator": (0x1aa, "18246f4bec33ea91f21f670c87a4bbccc4f1ccbf7ed9f60bc34d07ba2f133a25"),
    "__ZN16IGAccelMemoryMap21freeGPUVirtualAddressEv": (0x10a, "ff45337bab3d4a8e8a5018d739955a37a365ffbba6a49a1d9f4ed0796b2cceb8"),
    "__ZN24IGStolenMemoryDescriptor12setPurgeableEjPj": (0x2a, "97baec040a0d4c7f45075fefae44847895641f129d598b41581dceb613be1b9e"),
    "__ZN18IGStolenMemoryPool8allocateEm": (0xa8, "2da22d3ece5349942f8b06df08644420dc1824d5c8059116ba3e107214a46875"),
    "__ZN18IGStolenMemoryPool10deallocateEP24IGStolenMemoryDescriptorym": (0x8e, "27b8b16ab322e5396a7108e0c7ef4831662f3ac1873a6e949762fc2504b5f58a"),
    "__ZN24IGStolenMemoryDescriptor12withSubRangeEP18IGStolenMemoryPoolP18IOMemoryDescriptoryyj": (0x88, "5158c6c9238bdd55360ab52313b3fd1458a22651c01734be688d09aad671de9a"),
    "__ZN18IGStolenMemoryPool5purgeEv": (0x32, "f001f9826c7d49fe1748b47b97f5850bcb8c1cef18405b7088f93573526c9f9b"),
    "__ZN17IGInterruptBridge15systemWillSleepEv": (0x2c, "e6e531af30a1da358a33e447f4007ba1d2beb51b13f05ebb9d2b1f2506116449"),
    "__ZN17IGInterruptBridge13systemDidWakeEv": (0xa, "fd901862e71d5dfd92db1285d1f904a2a2edc3b097f8bbeade9271fbb0cf870c"),
    "__ZN12IGScheduler415systemWillSleepEv": (0x12, "ebc6258ca855696d94fdaa3df1e2006f5549fb32d7b70d049dd2fa51a16bcd19"),
    "__ZN12IGScheduler413systemDidWakeEv": (0x12, "e0172f9154cee45d45aaa5ca84ea00dc0a92a63227809759b44478965e9a56ae"),
    "__ZN11IGScheduler15systemWillSleepEv": (0x52, "42a0d04c4318678cf073b9cbe5ccc96f98b7452db1915b4a99e358e0fa85bfc0"),
    "__ZN11IGScheduler13systemDidWakeEv": (0x6, "5a96d1fb661d55552184ea24023ae8190bd1523ae1f855a8d671b07143e8b1df"),
    "__ZN16IntelAccelerator15systemWillSleepEv": (0x64, "37b3d5f2b38d0ba7a61820c7e0c24f593101fc0cf521a9380e1c3047b8e6f363"),
    "__ZN16IntelAccelerator13systemDidWakeEv": (0x62, "4f76040d34525555af43b282b2dd5c3ccaca5fa15dcc718f227151753aa8f4f0"),
    "__ZN21IGHardwareGuCCTBuffer32handleSoftwareGuCToHostInterruptEv": (0xa4, "4e3a72792d35aff7c4ee3b1dd14b91de487b2717801c827f683d4e62e4c0f4bc"),
    "__ZN31IGHardwarePerProcessPageTable324freeEv": (0x80, "c5f1bb69bf4cedcdc999c0f6c34cc47167346196816f14db050a18609bb9893c"),
    "__ZN31IGHardwarePerProcessPageTable644freeEv": (0x1d4, "b3d0469c1f6405f8f9e5f77055fb7338fe457582600e16fba07baf10dd6fd165"),
    "__ZN29IGHardwarePerProcessPageTable4freeEv": (0x12, "6e7702f85ee359086167a27efd1347edb4b2ce757dcd7c6c91cb4c8c66c268aa"),
    "__ZN19IGHardwarePageTable4freeEv": (0x12, "2055826c29bb9708f5dce7f67f665c461226732dd4a3ab43ae624fd64fb478fc"),
    "__ZN11IGAccelTask4freeEv": (0xe8, "0ae2167df94317690656c9858eb1918b8bdc54b5737f367d0fea768a10e99a07"),
    "__ZN17IGHardwareContext15initRingControlEb": (0x62, "09abdc95cf3157992522b47053e289a5c2b476953853c0acf4f08eaab5dec6ca"),
    "__ZN18IGAccelFIFOChannel18submitRingCommandsEPjjj": (0x106, "1e7d84456ce9eda587497696e280fcf28591b3bb9af1a7cff339de340db7cc86"),
    "__ZN20IGHardwareRingBuffer9alignRingEj": (0x28, "c9612928d88c4157c5d80847c6e8a8123ae227b7b08a8879b54f5387b285a2ee"),
    "__ZN20IGHardwareRingBuffer14submitCommandsEPjjj": (0x5e, "922dbcc17b815c68b6bf5535ef09feaabac1d7dd0c63015fbe0f2e15a27dd8e0"),
    "__Z15utilGetPropertyIjET_P15IORegistryEntryPKcS0_": (0x18c, "9da1339c8d7b6f93f71bf4020792fc4090572cca3bb9046120eb4dc617ef28af"),
    "__ZN20IGHardwareRingBuffer4initEP17IGHardwareContext": (0x1ec, "34cc30b4c471c23a0790bc8de98ddafe0aa25606ba37fee065a30f3a6f1ee1c9"),
    "__ZNK17IGHardwareContext17getRingBufferSizeEv": (0x14, "fe5061119174d816668edb9231f98b31fe3a7455d979e3f89d9467c538f4249a"),
    "__ZN20IGHardwareRingBuffer10writeQWordEy": (0x132, "ca328112ea77f150daa058f08cf7363e638748a85043c3c80777e25fffca2237"),
    "__ZN20IGHardwareRingBuffer11writeBufferEPjj": (0x16a, "d4fbab26bff0fec290ec13249eb2758f2bd491dbaabd1a2de12d85fdb7e06422"),
    "__ZN21IGAccelDisplayMachine16generateFlipWaitEP18IGAccelFIFOChannel": (0xda, "8ec7480e7b887eaeba9dbecc75dd1f366b9a761a95094f1a9f4cde80cfe9fb85"),
    "__ZN12IGScheduler416checkForProgressE10IGHwCsType": (0x8, "aaa500a73706124bc5374dc27c8b444160b15dc8a45b0fef9354b23106b76348"),
    "__ZN20IGHardwareRingBuffer19debugGraphicsEngineEv": (0x40, "ee36c90b746b8637259c7893cd17cd879316c7ae1d3b0c27687f9cd60bfdec74"),
    "__ZN20IGHardwareRingBuffer11waitTimeoutEU13block_pointerFbvE.cold.1": (0x12, "ef98956647d1ee0727b6b56636f23433333ceab809ec72318da1223c5400a8a2"),
    "__ZN20IGHardwareRingBuffer11waitTimeoutEU13block_pointerFbvE": (0x106, "caa2ccd4ce6a411ac4daa58d3b037dd5a561a418c8169e0b824d30a04bc8e1fb"),
    "____ZN20IGHardwareRingBuffer12waitForSpaceEj_block_invoke": (0x13, "4b735a005272c87b450984233e364446d5611e1f76d63b4ca057ea5d7b54c946"),
    "____ZN20IGHardwareRingBuffer12waitForSpaceEj_block_invoke_2": (0x16, "53c7f4d2d02474e67d709ce68249d2d01e9989720f65936f8df3e9d2ba563d46"),
    "____ZN20IGHardwareRingBuffer12waitForSpaceEj_block_invoke_3": (0x17, "6b36cfc2db80a58536fcdce6fbc358f83b3358b33da57e50ad59816a9f339554"),
    "__ZN20IGHardwareRingBuffer12waitForSpaceEj": (0x3d0, "6de374e33ab0cb73879b30e9102f651e2236371295f9575df7aba524d6cddc18"),
    "__ZN20IGHardwareRingBuffer16getFlushTLBSpaceEv": (0x20, "371c66eaa0eb7f33572bfeca005edcd13b44d6cb1f1cb2f8a92b3baa8bbb5c26"),
    "__ZN27IGHardwareRingBufferCompute16getFlushTLBSpaceEv": (0x20, "e60720b063a6a465489f7c5594d97f92c14bfe0122ae41a6bcea00d8fb66f380"),
    "__ZN24IGHardwareRingBufferMain16getFlushTLBSpaceEv": (0x20, "e60720b063a6a465489f7c5594d97f92c14bfe0122ae41a6bcea00d8fb66f380"),
    "__ZN20IGHardwareRingBuffer10writeDWordEj": (0x136, "ebcb4164727daaf22fd9df83aadbf2c68de0a8024df79bb7fcc1c3f1c1790d47"),
    "__ZN20IGHardwareRingBuffer16writeFlushAuxTLBEv": (0x46, "a6886597ded0b5439e645aa2f9de563b1b94241c12c7c7696a396759d4505a31"),
    "__ZN16IntelAccelerator32flushHardwareAfterGttUpdateOfAuxEv": (0x16, "bf1c2cfda9f70eb29f938d9699cd21e4a10afebc12bf15912310386579804d6a"),
    "__ZN20IGHardwareRingBuffer13writeFlushTLBEv": (0x116, "047d585e1a417ff67d8de8d761ba42e15f3a7206bf224a1b51bdaf5599117110"),
    "__ZN27IGHardwareRingBufferCompute13writeFlushTLBEv": (0x172, "c42d172fd9503b115c3b3196a7138eebff9cd3f9a4a7e717acf7941f61bb8f79"),
    "__ZN24IGHardwareRingBufferMain13writeFlushTLBEv": (0x172, "006309753fdb2c5ab236aa577eafedd7f901677a07e7acd6a8d7b24cd97b6ab0"),
    "__ZN31IGHardwarePerProcessPageTable6411expandLevelINS_10LevelEntryILm9E21GTTPageDirectoryEntryEENS1_ILm9E28GTTPageDirectoryPointerEntryEEEEbRT_yyPT0_ym": (0xde, "4c0a775e56fefaf5fd0b970cf396668811bc58ca3b3c882d57c76e4a0653df80"),
    "__ZN31IGHardwarePerProcessPageTable6411expandLevelINS_10LevelEntryILm9E17GTTPageTableEntryEENS1_ILm9E21GTTPageDirectoryEntryEEEEbRT_yyPT0_ym": (0x98, "2f377f041a36bf2f733a4e34d25d239d256f9bd57e8445837aec60170c9235d4"),
    "__ZN19IGHardwarePageTable12releaseRangeERK14IGAddressRange": (0xd4, "7fb7fa5dd40ed422c9a2aa219d8e0c78dfcd35bfdeaaa77d27ad3c193cfb9489"),
    "__ZN16IntelAccelerator27flushHardwareAfterGttUpdateEv": (0x16, "0c05a6864ebec38017506837721a541f874f1c17997dcba65ef00919e3a3b18f"),
    "__ZN31IGHardwarePerProcessPageTable648mapRangeERK14IGAddressRangeyy": (0xe6, "f58bafe5bf67319aed0ecd8e44965f6d2dab044b6963b3a36d52a9d5a733cdba"),
    "__ZNK31IGHardwarePerProcessPageTable649pageWalk3E19GTTVirtualAddress64RPNS_10LevelEntryILm9E17GTTPageTableEntryEE": (0x68, "78eae8bb2011882715ef02e83b19e0514736d601ac53d7a176a26cf386cb86b6"),
    "__ZN31IGHardwarePerProcessPageTable6410unmapRangeERK14IGAddressRange": (0xbe, "d06c36dcf6a74a6400f97dd245ff6289685ea468f5f29453dab8122a8dec457a"),
    "__ZN31IGHardwarePerProcessPageTable6413mapRangeDummyERK14IGAddressRangey": (0xe4, "e12a44913420fa13088c2bcee80915e2e6d44ee6fa875c40ac43843d1feac884"),
    "__ZN31IGHardwarePerProcessPageTable6411expandRangeERK14IGAddressRange": (0x246, "d0b93020e89cf47d6b877b51cdac3640699624693bcf7a368d593227443fd89b"),
    "__ZNK10IGPagePool9MetaClass5allocEv": (0x40, "65f06ebfe980773f1197b135a255e6293f189a643e11488d8b7953cd51fdca69"),
    "__ZN10IGPagePoolC1EPK11OSMetaClass": (0x20, "48ed1831fa6b10b5f986207d9ce46af1ba0068f4e81aaabc938eae817c9ae9c2"),
    "__ZN10IGPagePoolD2Ev": (0xa, "aafd66af2c321a1032ffdbaea51ef446e7df7cd4b20fe53e4fdf6af362acadb2"),
    "__ZN10IGPagePoolD1Ev": (0xa, "aafd66af2c321a1032ffdbaea51ef446e7df7cd4b20fe53e4fdf6af362acadb2"),
    "__ZN10IGPagePoolD0Ev": (0x22, "b405fb72aec724a6adf49da0d176239a1ad6a1c7fd78469f09e809bb4544e864"),
    "__ZN15IGMemoryManager19releaseDeviceMemoryEv": (0x46, "85eb115d6987c20d6b921fce637404510616ef4846cff455cc0c76cb43fa635c"),
    "__ZN15IGMemoryManager15releasePagePoolEv": (0x92, "d23b081e493b13dbe74745d7618b2b5c8122e7f0ad8ad6ecf7bde07f532e433c"),
    "__ZN15IGMemoryManager4initEP16IntelAcceleratorRK18IntelSharedMemInfoRK14_stolenMemInfo": (0x232, "4dec40e7229fc980e61dee463496e387f893fd06789f6841a5dafd5dff572347"),
    "__ZN15IGMemoryManager12initPagePoolEv": (0xcc, "f5a5d43004dbecc5719cdc76507ee529775658060db9aea3fbdf6256f813b176"),
    "__ZN15IGMemoryManager4freeEv": (0xb4, "69f568f6fe6e36f88666926f823d5b61677d984d47ff6095fc06cf148e6714b6"),
    "__ZN15IGMemoryManager14registerEventsEv": (0x50, "3fd8b2a3929eb106d81bb314a22376d3218f1865d8842a9e33f6cca920e46cf0"),
    "__ZN10IGPagePool11withOptionsEP16IntelAcceleratorj": (0x4e, "2a792cbfe2276f7349bc1eee5f550cde74c3f6b916793da473a59ad28307e0a6"),
    "__ZN10IGPagePool10pruneEventEP22IOInterruptEventSourcei": (0x46, "17ea1938584ddf61ac147cd4b6f164e8b9ddae802a9d4f2cdf3a8c7f825e2c8f"),
    "__ZN10IGPagePool10pruneTimerEP18IOTimerEventSource": (0x4e, "45fc274f9ca594a9bf136f3c25a7c34e3bca0ed4141305a4a5c21df4d221c1d3"),
    "__ZN10IGPagePool11inPruneListEPNS_11PoolElementE": (0x24, "af1c4fb1f9fa50011285cfce15061aaa638e230c75daff90e3c6b6601d86156f"),
    "__ZN10IGPagePool14registerEventsEv": (0xc0, "a7a62cd8bda261e217a338827df271f23f306a7a07a83fc3b442fc0d55aa2356"),
    "__ZN10IGPagePool25describeDriverAllocationsEP21IOAccelAllocationInfoi": (0x6, "5a96d1fb661d55552184ea24023ae8190bd1523ae1f855a8d671b07143e8b1df"),
    "__ZN10IGPagePool15initWithOptionsEP16IntelAcceleratorj": (0x174, "fc32202718c77bc993d98568900c4e0490875f6d4c560e300de1faebcf705e24"),
    "__ZN10IGPagePool4freeEv": (0x114, "115a4127e4b6a8b646d8410e411e9c57ed1d3acacbddd0c4089365b49b928de6"),
    "__ZN11IGHashTableIPN10IGPagePool11PoolElementEmNS0_15PoolElementHashE25IGIOMallocAllocatorPolicyED1Ev": (0x120, "0a79a5b0a2d2e0da123f2e24275266707d7563ad24287d6f24f721fd3d62076a"),
    "__ZN11IGHashTableIPN10IGPagePool11PoolElementEmNS0_15PoolElementHashE25IGIOMallocAllocatorPolicyE6removeERKS2_": (0xc3, "ea3cd8f81479b491152b5bd65341df215a93b33d4fc8de7391ab90cd6833339b"),
    "__ZN11IGHashTableIPN10IGPagePool11PoolElementEmNS0_15PoolElementHashE25IGIOMallocAllocatorPolicyE14shrinkIfNeededEv": (0x52, "ff90c793387645bee297503014bca4535e28a60556297cf57a295db08f95efdf"),
    "__ZN15IGPriorityQueueIPN10IGPagePool11PoolElementENS0_18PoolElementCompareENS0_15PoolElementHashE25IGIOMallocAllocatorPolicyE13percolateDownERKS2_mm": (0x15a, "7426afabddba0c53420eda649880faeee5706a4ba8ede1526a98022d9ef6fad7"),
    "__ZN11IGHashTableIPN10IGPagePool11PoolElementEmNS0_15PoolElementHashE25IGIOMallocAllocatorPolicyE15resizeAndRehashEm": (0x170, "8133ad3b23eeae7e622f6bb07c2a48c1ee560f07300104a8618c00449541112a"),
    "__ZN8IGVectorIN11IGHashTableIPN10IGPagePool11PoolElementEmNS1_15PoolElementHashE25IGIOMallocAllocatorPolicyE4SlotES5_EC1Em": (0xa2, "7b753decc920c14c6625a45e26fb343ea9f1a192b1e5fbc3b870c64a64bdf259"),
    "__ZN8IGVectorIPN10IGPagePool11PoolElementE25IGIOMallocAllocatorPolicyE4growEm": (0x78, "d0eadd8fc2d8b9227e151fa3b7f5e650f0be03ffd879463ac349f75217c6c440"),
    "__ZN11IGHashTableIPN10IGPagePool11PoolElementEmNS0_15PoolElementHashE25IGIOMallocAllocatorPolicyE3addERKS2_RKm": (0x120, "a28254e0b16c65ea6d5499fb072371a87ea32636bf833a1be0b8458070dcd689"),
    "__ZNK11IGHashTableIPN10IGPagePool11PoolElementEmNS0_15PoolElementHashE25IGIOMallocAllocatorPolicyE8containsERKS2_": (0x56, "4c27852052d7d9fdf831c256199644240d324dc8acd5bc05330f41e50dc92124"),
    "__ZN11IGHashTableIPN10IGPagePool11PoolElementEmNS0_15PoolElementHashE25IGIOMallocAllocatorPolicyEixERKS2_": (0x56, "057b3616612957616516b8c0959418666e431b7dd67a029f3ecedb17885dc04b"),
    "__ZN10IGPagePool4growEv": (0x52e, "b6dd584c6c29ec5a49e518753c2613534a413ce25ebacce80b78c4abb8086ae0"),
    "__ZN10IGPagePool12allocatePageEv": (0x1f2, "f5510c1459a78d42faa38e262fb7a7514f52974ecc184ecedd9e31b8c2c4e851"),
    "__ZN10IGPagePool5pruneEj": (0x436, "5fc5da8153de076b57dfb6b217037f4421b4d88f7be6b080035fccd33764d605"),
    "__ZN15IGPriorityQueueIPN10IGPagePool11PoolElementENS0_18PoolElementCompareENS0_15PoolElementHashE25IGIOMallocAllocatorPolicyE4evalERKS2_": (0x18a, "c09752525356d466e3948b5ef023111d0f576807cb04ddefb40db7ff0a3d8af9"),
    "__ZN10IGPagePool11releasePageEPKNS_14PageDescriptorE": (0x15e, "c0517b90af5a3ee2250dec98476192459c0da84b78fc8a33efc230d8c0b45822"),
    "__ZN10IGPagePool13schedulePruneEv": (0x4c, "4823a25c10061da7466412539c82cd1eda14b5bdea7cf8cdcc267c624e300d27"),
    "__ZN31IGHardwarePerProcessPageTable6411expandLevelINS_10LevelEntryILm9E21GTTPageMapLevel4EntryEEvEEbRT_yyPT0_ym": (0x9e, "0ea77cb30a3a0c26fcfc21c3a1abdfe148a3689ce57bdbf6843db6684b8b190b"),
    "__ZN31IGHardwarePerProcessPageTable6411expandLevelINS_10LevelEntryILm9E28GTTPageDirectoryPointerEntryEENS1_ILm9E21GTTPageMapLevel4EntryEEEEbRT_yyPT0_ym": (0xde, "064ba497cfafbe98e4aca310a5ccc2670fa74dd180e01c5fc994116b06b46f05"),
    "__ZN31IGHardwarePerProcessPageTable6411shrinkRangeERK14IGAddressRange": (0x2d0, "4998b7b753950de7880477e43dc13e5eb5329decc2b9e1cc416c8f0f42ad42dc"),
    "__ZN31IGHardwarePerProcessPageTable6411shrinkLevelINS_10LevelEntryILm9E17GTTPageTableEntryEENS1_ILm9E21GTTPageDirectoryEntryEEEEbRT_yPT0_ym": (0x86, "2e46f3208e230b8bbbf52502b09dac6961c4eb8468daa1f7f673c3c4770e2187"),
    "__ZN31IGHardwarePerProcessPageTable6411shrinkLevelINS_10LevelEntryILm9E21GTTPageDirectoryEntryEENS1_ILm9E28GTTPageDirectoryPointerEntryEEEEbRT_yPT0_ym": (0x86, "6b02ec67cd079a50395b3f5450536af287dcb4d957c517c6250536893f46f754"),
    "__ZN31IGHardwarePerProcessPageTable6411shrinkLevelINS_10LevelEntryILm9E28GTTPageDirectoryPointerEntryEENS1_ILm9E21GTTPageMapLevel4EntryEEEEbRT_yPT0_ym": (0x86, "0cbb866397e6b36620f1efae900e78218fc4a7c9e846a6e10fe9238a8f77df48"),
    "__ZN31IGHardwarePerProcessPageTable6411shrinkLevelINS_10LevelEntryILm9E21GTTPageMapLevel4EntryEEvEEbRT_yPT0_ym": (0x4e, "3060c61f22633fe9b0ea1bdce718028912bbd26d27824aaee6cc1034ce390aa5"),
    "__ZN19IGHardwarePageTable15initWithOptionsEP16IntelAcceleratorNS_4TypeE": (0x3e, "4dd2d3d12a42751fe3a58852b7c9b758ebe81cf80aa32015c9550db78070356f"),
    "__ZNK11IGAccelTask29getHardwareContextAddressModeEv": (0x16, "009814216381ad12a9161899b0e332c8630311c31f5307a937f0c1b63f5a022b"),
    "__ZN31IGHardwarePerProcessPageTable6421mapDescriptorForRangeERK14IGAddressRangePN10IGPagePool14PageDescriptorE": (0xb6, "e0e111abf4ec5a2de14ca65d1304611a6c4990f5da6decb5abaa1a29d4fe78c6"),
    "__ZN31IGHardwarePerProcessPageTable6423remapDescriptorForRangeERK14IGAddressRangePN10IGPagePool14PageDescriptorE": (0x8a, "eb27e5cf9f9a519893647c044cae0fb8d967f098701a46d3cd9dca9fac0ff9ee"),
    "__ZNK31IGHardwarePerProcessPageTable6422readDescriptorForRangeERK14IGAddressRangePPN10IGPagePool14PageDescriptorE": (0x32, "fb2644014513475491f0504737f0fe210e37e4635d3200f2a973539e907909bf"),
    "__ZN31IGHardwarePerProcessPageTable647expand2E19GTTVirtualAddress64": (0x84, "b92e3f9f67a7d71b4b2a63a4d3ed2c48e25e703a0e966146d9592e546bfac44a"),
    "__ZN10IGPagePool14PageDescriptor6retainEv": (0x14, "1b3cd9862b970917206f59e670062a08d6a5eedd1362ad697c95b7ffd84b5c0c"),
    "__ZN10IGPagePool14PageDescriptor7releaseEv": (0x38, "97af69e37be7b19aab2f4769dc243f9759121e84f57eada2e57846a27b54f2ac"),
    "__ZN29IGHardwarePerProcessPageTable15initWithOptionsEP16IntelAcceleratorP11IGAccelTaskj": (0x3a, "e306e97f7f433d7cbb1f3bc321fe44096cefd96c1975d336d0ea7073090feb83"),
    "__ZN31IGHardwarePerProcessPageTable3215initWithOptionsEP16IntelAcceleratorP11IGAccelTask": (0xbe, "7002d2e902245add7b5bca2f3c598f7a2122a1aa9753fa2838d486ad3460c106"),
    "__ZN31IGHardwarePerProcessPageTable6415initWithOptionsEP16IntelAcceleratorP11IGAccelTask": (0x40, "3460b8dba11c7deb5f0c32b5eb5822290092ced82dd1d065d24be4b7ec6aa5fb"),
    "__ZN11IGAccelTask27releaseManagedPageTableListEv": (0x6e, "0087a143da10c4a5bd1efb713814ca5e18a990f5e4fea35b8d1da254ad45a75c"),
    "__ZN29IGHardwarePerProcessPageTable15synchronizeWithI25IGHardwareGlobalPageTableEEvPKT_RK14IGAddressRangeb": (0xa, "aafd66af2c321a1032ffdbaea51ef446e7df7cd4b20fe53e4fdf6af362acadb2"),
    "__ZN29IGHardwarePerProcessPageTable15synchronizeWithIS_EEvPKT_RK14IGAddressRangeb": (0x42, "09fcf7f1db075ec15752cef118f8cf28ef5ef334b4a1d48a899470e250d70a2c"),
    "__ZN29IGHardwarePerProcessPageTable20synchronizeEachEntryEPK19IGHardwarePageTableRK14IGAddressRangeb": (0xd0, "6dc9a007f31f74b1e2f38b04cb4d64fe92e439e02998db60862074edfcd07b65"),
    "__ZN29IGHardwarePerProcessPageTable25synchronizePageDescriptorEPKS_RK14IGAddressRangeb": (0x5c, "e952223bccb90c24357deabea67ade3f8f62f757a4e797b2d6abce769764d1c2"),
    "__ZNK25IGHardwareGlobalPageTable4readEyRyS0_": (0x44, "5cc6a86d9a27cf1ee2f28388b3542d08102ffd35a4bc6db509e3cd5dd2d71ccd"),
    "__ZN11IGAccelTask24initManagedPageTableListEv": (0xfc, "4b026fd8979c2010b304895b2d6f61c69167e83923cb7070cdc238b179d45e77"),
    "__ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask": (0xa6, "9f8b0a4af92e2da84cac933655cc33c6ed7ed5a31236a73a91955d91ba76d911"),
    "__ZN15IGMemoryManager22updatePageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap": (0x11a, "9b574d3a8f1ae07327da68f84f4eb99a08e8ea154a596f723ae7f558ffdf46a2"),
    "__ZN16IGAccelMemoryMap18updateGPUPageTableEv": (0x140, "75ba5674583a8d27d54acab18290e78bbc9de8157b19bfc6614c379d39fa3fbc"),
    "__ZN16IGAccelMemoryMap15updateCacheTypeEj": (0x24, "0c704192e43ed19c39a2179ea6e80551a07af30a8a541016a913f3d9572516f8"),
    "__ZN15IGAccelResource22updateMappingCacheTypeEj": (0x30, "499c98e59e89b29b95b7d14247a756db3f980b5f58134dcf2c49c3baf5db97bf"),
    "__ZN23IGAccelSharedUserClient13depth_resolveEPvy": (0x52a, "ddb1d9367ac5a3047151e18fd6433553e6e3529559f2ec58f84e450b89ad52aa"),
    "__Z19depthStencilResolveR16IOAccelResource2RKN14IntelMTLRender20sResolveResourceDescEP11IGAccelTaskR16IntelAcceleratorPN15IGAccelResource17ResourceInfoEntryEbb": (0xa0, "821adafde1ed4acb197457b8b4ad613a883ed6f392f5fd4fc3cce682a64c7864"),
    "__ZN16IntelAccelerator18submitDepthResolveEP22depth_resolve_params_tP11IGAccelTask": (0x1f6, "79b934f887406d3ad54a541be2de697328137c7af5d43350253231713f8f9987"),
    "__ZN15IGAccelResource18submitDepthResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask17EIntelResolveTypehhtt": (0x3f0, "a3f33094b424e909e708e33b10b3be2d6abf6e3682dda5fa273331de867febde"),
    "__ZN23IGAccelSharedUserClient12bindResourceEP15IGAccelResource": (0x48, "51d8bc7e5b290db9452dc62000c768ece7c42a3fc767938d2592a73e4323a820"),
    "__ZN23IGAccelSharedUserClient13color_resolveEPvy": (0x5d0, "8e09b7dcbb4d03aa0fc6eb10ae14a2db460543bcaf13d347ddb43fd3e457ef94"),
    "__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveType": (0xa6, "bb27f13ada6c05e268f7f12e4089c601d90b946e041311e42d965199f618477f"),
    "__ZN15IGAccelResource36enableRenderCompressionWithAccelTaskEPNS_17ResourceInfoEntryEyR16IntelAcceleratorP11IGAccelTaskhhb": (0x2ac, "13fbc4f7a1fded75ad34c5699be050bf8b472fa635893b2ff6dfd5065f9cc87d"),
    "__ZN16IntelAccelerator20barrierForWaitEventsEbP18IGAccelFIFOChannel": (0x52, "1714d3e9be9868c3c9db6a8bd6cb8a2e8f3fc83a9acf2c82ba8e9f2d72ff57d8"),
    "__ZN19IGAccelEventMachine11finishEventEP12IOAccelEventj": (0x70, "897fdb00bbbe43b901ca0af633003863b20d2e1b2e16646bfece3bb92d6a9850"),
    "__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveTypehh": (0x554, "a10208dea187f2099cf0deab208c392b50c600d292f2604d6fffea3b7aacc528"),
    "__ZN16IntelAccelerator16submitCCSResolveEP15IGAccelResourceP22color_resolve_params_tRK8IGVectorI11blit_rect_t25IGIOMallocAllocatorPolicyEP11IGAccelTask": (0x2b0, "66bb41fb30a7dbf3b678fbae69ddb8b72a5f825b96042ce1a643eaf13c54c6c0"),
    "__ZN19IGAccelVideoContext30updateResourceMappingCacheTypeEP15IGAccelResource": (0x24, "91ca56bc8ad407f81ad7472060497e0ddb88b32259389dba252e9c29890d9d02"),
    "__ZN19IGHardwarePageTable11updateRangeERK14IGAddressRangePK16IGAccelMemoryMap": (0x3c4, "7bd38a56c02637892a4672882eed36a3bea60b0b6ee6017982a0760713713b92"),
    "__ZN19IGHardwarePageTable11commitRangeERK14IGAddressRangePK16IGAccelMemoryMap": (0x41c, "e063629df4a8d16d85cf3d1b599c036372c0763b560a6a35289d488d80ac410a"),
    "__ZN15IGMemoryManager26commitIntoPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap": (0x11a, "a433c1af43e1fbac1d82da400c6ec1857881e6385c07019d782441fe209713d1"),
    "__ZN16IGAccelMemoryMap22commitIntoGPUPageTableEv": (0x140, "b8318f92ed0a3a62eb086877f6f5a65e0c32cc53d610bcad69540967279189e4"),
    "__ZN16IGAccelSysMemory4wireEv": (0x10a, "ec07edd1fdb32fe38f4470a0336602acb181631387f8f3f32c3220a926b0b580"),
    "__ZN16IGAccelMemoryMap23releaseFromGPUPageTableEv": (0x140, "7abf8665679b8628fe1472f77fd9dc87a03b4bbafa380c28b7efe11151938168"),
    "__ZN15IGMemoryManager27releaseFromPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap": (0x112, "d0d45163270d1b779be9bedfd1a7431bb4750ea2a7380dc4f6d1c4c5c1ab28eb"),
    "__ZN16IGAccelSysMemory6unwireEv": (0x10e, "40fb9a8f5b8423fe9e34ad3d4531391e36ce95bd6ee7b3105a12abbb0bb89843"),
    "__ZN16IGAccelMemoryMap4freeEv": (0x12, "748257c15ee1b1bfcb9e08da1cc2f7ad2611bd715dae5f926581ce36fa9ae057"),
    "__ZN15IGAccelResource8completeEv": (0x6c, "ccc8dc301c086c4dac5eb2c70f24f454bdfb37c6055c82b68ee2bfdc199b7afb"),
    "__ZN19IGAccelEventMachine10writeStampEiP17vendevtCommandRecj": (0x22, "005d2c2cfb037d2b79faf330183d7084d75493490ef638f9f5e4f9801f0fc41f"),
    "__ZN19IGAccelEventMachine14getStampOffsetEi": (0xc, "4d9f0b18e011c0d3e92c48d0e9a837c7b96e70c05f23beb8bb8dae7a0625504a"),
    "__ZN18IGAccelDisplayPipe16beginTransactionEP12IOAccelEvent": (0x4e, "afaf2c261837e01c0ea05f84cb30e10cf3225d82381602b9e73274631d146ae9"),
    "__ZN18IGAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2": (0x5e, "883521e4857160a449dccb37298cfed86801910a5cbe0dcbacefd0312b25f2e2"),
    "__ZN19IGAccelEventMachine21enableSchedulerEventsEv": (0x40, "f8f0e4f69fbcd568efb5443e8e745994d3182412ce07a855f0cfd931a426d004"),
    "__ZN26IGHardwareCommandStreamer54initEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler510IGHwCsType": (0x224, "8f48a87d0813d09e9c4062970a48537d36182ffc2c9ac4a56894f5cffb075bc8"),
    "__ZN26IGHardwareCommandStreamer521registerForInterruptsEv": (0x90, "273bc67e508714d78441d1723972f3329308a1dc01e2b3e9616f6703a52e6370"),
    "__ZN26IGHardwareCommandStreamer528enableContextSwitchInterruptEv": (0x36, "e79036987a975eeab70e8fceb4c54e52685d7d67dd27cc03830dfbbe0fe6734d"),
    "__ZNK11IGScheduler11getWorkLoopEv": (0x12, "895c706830ac837dfdb730fc602937effaf5fc188add2c21937b12ffad12bb38"),
    "__ZNK12IGScheduler511getWorkLoopEv": (0xd, "a384252c609ebb926362d345822bc7a0958dd681ff28627d11d57f490e7eb8c4"),
    "__ZNK5IGGuC11getWorkLoopEv": (0xd, "082091370da302c29650b27b4d2ebff8c0731a022eb0c755ee4ee9689b5064c0"),
    "__ZNK22IOGraphicsAccelerator211getWorkLoopEv": (0xe, "0ef049497a03533ecd154fb19d6db1634b48a484a93868849084e2b9c759fd8c"),
    "__ZN19IGAccelEventMachine4initEP22IOGraphicsAccelerator2ji": (0x64, "428bd74bf188a21082140b017f96719913dbe234d960d08effc6fada46de622d"),
    "__ZN19IGAccelEventMachine4freeEv": (0x66, "69c056b5678884fc01cb87e1a50e94a28a35a7d524b4730bc7d845a02a37af14"),
    "__ZN19IGAccelEventMachine29handleSchedulerStampInterruptEP22IOInterruptEventSourcei": (0x18, "c0cf1d2c8eeec02c61b149219a63f79263fc3912b9553803a5ff277ea7a596d1"),
    "__ZN16IntelAccelerator17signalStampUpdateERK8IGBitSetILm64EE": (0x202, "1977013388b8e6a38317670d008d86e63a766e914054ab6b3df406e7b9c1ea45"),
    "__ZL21getInterruptTypeIndexj": (0x2dc, "a437c9f3b0f1652461adcbd7dd5733d37fb70fd8b841d71e6678331312a3979c"),
    "__ZN19IGAccelEventMachine20enableStampInterruptEi": (0xc, "d36367a855e7c70648ad4cebd97046b4cd0f93b1ea460faad009a1a3d8676db9"),
    "__ZN19IGAccelEventMachine20enableStampInterruptEii": (0xb6, "a1258420942f61394025ad599519cee464bada81bc3d4779eb45ed0c25cade22"),
    "__ZN19IGAccelEventMachine21disableStampInterruptEi": (0xc, "d36367a855e7c70648ad4cebd97046b4cd0f93b1ea460faad009a1a3d8676db9"),
    "__ZN19IGAccelEventMachine21disableStampInterruptEii": (0xf4, "f4047aac956b154a0482d2853c70ebf184a1dbcd936ece39c5517cfd9f8f30ea"),
    "__ZN12IGScheduler420enableStampInterruptEi": (0x46, "8318558af6a4c8679bbd3a24a4253d7e0fd8463cc6b75c518c3f2c6694351b73"),
    "__ZN12IGScheduler421disableStampInterruptEi": (0x46, "cdc35acba686d6ee1103ab366372f0e051ab83ea9002ff48c591e28bd17ccd60"),
    "__ZN26IGHardwareCommandStreamer420enableStampInterruptEi": (0x11e, "4c7dc2ad3f870ec4208a4d1e1e7cb9af47cc13f6c1376a81859599e0fe4f1e81"),
    "__ZN26IGHardwareCommandStreamer421disableStampInterruptEi": (0x120, "c92525548bf13a3f096298f184bc5134814edb8cdd0b764c54a58795f8ed056f"),
    "__ZN17IGInterruptBridge15enableInterruptEj": (0x3c, "8e518e0554ae427eb88ec58df690f698f25c7e3b50086768221770d1ebae2b21"),
    "__ZN17IGInterruptBridge15enableInterruptEPNS_15InterruptTraitsE": (0xd0, "19dc8231d30e33d7c5fbcab1bc43267a749a3e74d6d24a69f8faab51c8e74081"),
    "__ZN17IGInterruptBridge16disableInterruptEj": (0x3c, "87989129774e6e26990dbe789721201da8732b452891d20529c9f5e0b48be103"),
    "__ZN17IGInterruptBridge16disableInterruptEPNS_15InterruptTraitsE": (0xd0, "28dd612dc90781aa9a0ca8938efed1d55a95c0eb4addf7b88478ab5b3e2591d4"),
    "__ZN11IGScheduler33enablePeriodicEventTimerInterruptEP22IOInterruptEventSource": (0x78, "030b488c557f865c52343ba7561d7b084e109914a9894b9fab5c8c8b06c3cb79"),
    "__ZN11IGScheduler34disablePeriodicEventTimerInterruptEP22IOInterruptEventSource": (0x64, "6ad9c37406ce3699146f34cc7a6a3ce77366e1f53bd356b34294d55eb23a82a1"),
    "__ZN11IGScheduler28handlePeriodicTimerInterruptEP18IOTimerEventSource": (0x9c, "9c102a04de5a017636908823772724ea406316bc2142c0e8f2caf775809ec441"),
}


ENABLE = "__ZN22IOGraphicsAccelerator217enableAcceleratorEv"
DISABLE = "__ZN22IOGraphicsAccelerator218disableAcceleratorEv"
START = "__ZN16IntelAccelerator19startGraphicsEngineEv"
STOP = "__ZN16IntelAccelerator18stopGraphicsEngineEv"
ACCELERATOR_START = "__ZN16IntelAccelerator5startEP9IOService"
ACCELERATOR_STOP = "__ZN16IntelAccelerator4stopEP9IOService"
EVENT_FINISH_ALL = "__ZN19IGAccelEventMachine15finishAllStampsEj"
EVENT_TIMEOUT = "__ZN19IGAccelEventMachine12eventTimeoutEi"
SCHEDULER4_CHECK_PROGRESS = "__ZN12IGScheduler416checkForProgressE10IGHwCsType"
SCHEDULER4_PAUSE = "__ZN12IGScheduler45pauseEv"
SCHEDULER4_RESUME = "__ZN12IGScheduler46resumeEv"
SCHEDULER4_PREPARE_RESET = "__ZN12IGScheduler415prepareGPUResetE10IGHwCsType"
SCHEDULER4_GET_ACTIVE = "__ZN12IGScheduler417getActiveContextsE10IGHwCsTypePP17IGHardwareContextPyS3_"
GUC_PAUSE = "__ZN13IGHardwareGuC14pauseSchedulerEv"
GUC_RESUME = "__ZN13IGHardwareGuC15resumeSchedulerEv"
SCHEDULER_HALT = "__ZN11IGScheduler19haltCommandStreamerE10IGHwCsType"
SCHEDULER_RESUME = "__ZN11IGScheduler21resumeCommandStreamerE10IGHwCsType"
ENCODE_DEBUG = "__ZN16IntelAccelerator15encodeDebugInfoE15IGTimeoutReason"
GATHER_GUC = "__ZN16IntelAccelerator16gatherKeyGuCDataEv"
GATHER_RING = "__ZN16IntelAccelerator17gatherKeyRingDataE10IGHwCsType"
GET_INSTDONE = "__ZN16IntelAccelerator16getInstDoneSliceE10IGHwCsType"
RING_DO_HANG = "__ZN20IGHardwareRingBuffer14doHangAnalysisEv"
RING_DUMP_HANG = "__ZN20IGHardwareRingBuffer16dumpHangAnalysisEv"
RING_DEBUG_ENGINE = "__ZN20IGHardwareRingBuffer19debugGraphicsEngineEv"
FIFO_DIAGNOSIS = "__ZN18IGAccelFIFOChannel26getHardwareDiagnosisReportEPj"
RING_DUMP_STATUS = "__ZN20IGHardwareRingBuffer14dumpRingStatusEv"
RING_DUMP_REGISTERS = "__ZN20IGHardwareRingBuffer13dumpRegistersEv"
RING_DUMP_REGISTERS_RCS = "__ZN20IGHardwareRingBuffer16dumpRegistersRCSEv"
RING_VTABLE = "__ZTV20IGHardwareRingBuffer"
FIFO_VTABLE = "__ZTV18IGAccelFIFOChannel"
RING_RESET_GRAPHICS = "__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext"
FIFO_RESET_REPLAY = "__ZN18IGAccelFIFOChannel22resetHardwareAndReplayEv"
FIFO_SUBMIT_STAMP = "__ZN18IGAccelFIFOChannel18submitStampCommandEv"
RING_SLEEP_STAMP = "__ZN20IGHardwareRingBuffer13sleepForStampEPjjj"
RING_WRITE_STAMP = "__ZN20IGHardwareRingBuffer10writeStampEjb"
RING_MAIN_WRITE_STAMP = "__ZN24IGHardwareRingBufferMain10writeStampEjb"
RING_COMPUTE_WRITE_STAMP = "__ZN27IGHardwareRingBufferCompute10writeStampEjb"
RING_COMMIT_STAMP = "__ZN20IGHardwareRingBuffer18commitStampCommandEjb"
RING_MAIN_COMMIT_STAMP = "__ZN24IGHardwareRingBufferMain18commitStampCommandEjb"
RING_COMPUTE_COMMIT_STAMP = "__ZN27IGHardwareRingBufferCompute18commitStampCommandEjb"
RING_WRITE_BUFFER = "__ZN20IGHardwareRingBuffer11writeBufferEPjj"
RING_GTT_WRITE_MODE = "__ZN20IGHardwareRingBuffer15getGTTWriteModeEv"
TASK_SCRATCH_GPU_ADDRESS = "__ZNK11IGAccelTask27getScratchGPUVirtualAddressEv"
RING_WAIT_SPACE = "__ZN20IGHardwareRingBuffer12waitForSpaceEj"
RING_WAIT_TIMEOUT = "__ZN20IGHardwareRingBuffer11waitTimeoutEU13block_pointerFbvE"
SCHEDULER4_INIT_PRIVATE = "__ZN12IGScheduler421initSharedPrivateDataEP17IGHardwareContext"
SCHEDULER4_CLEANUP_PRIVATE = "__ZN12IGScheduler424cleanupSharedPrivateDataEP17IGHardwareContext"
GUC_ATTACH_DESC = "__ZN13IGHardwareGuC29AttachContextDescToGucContextERK21SGfxContextDescriptor"
GUC_DETACH_DESC = "__ZN13IGHardwareGuC31DetachContextDescFromGucContextERK21SGfxContextDescriptor"
GUC_INVALIDATE_TLB = "__ZN13IGHardwareGuC13invalidateTLBEv"
CONTEXT_FREE = "__ZN17IGHardwareContext4freeEv"
CONTEXT_INIT = "__ZN17IGHardwareContext15initWithOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh"
CONTEXT_RING_GPU_ADDRESS = "__ZN17IGHardwareContext25initRingGPUVirtualAddressEv"
TASK_STAMP_GPU_ADDRESS = "__ZNK11IGAccelTask25getStampGPUVirtualAddressEv"
TASK_STAMPS = "__ZNK11IGAccelTask9getStampsEv"
TASK_INIT_STAMPS = "__ZN11IGAccelTask24initStampAndScratchPagesEv"
TASK_FREE = "__ZN11IGAccelTask4freeEv"
TASK_RELEASE_STAMPS = "__ZN11IGAccelTask27releaseStampAndScratchPagesEv"
TASK_RELEASE = "__ZNK11IGAccelTask7releaseEv"
CONTEXT_NOTIFY_COMPLETE = "__ZN17IGHardwareContext14notifyCompleteEP12IOAccelEvent"
FIFO_NOTIFY_COMPLETE = "__ZN18IGAccelFIFOChannel14notifyCompleteEP12IOAccelEvent"
RING_NOTIFY_COMPLETE = "__ZN20IGHardwareRingBuffer14notifyCompleteEP12IOAccelEvent"
EVENT_MACHINE_VTABLE = "__ZTV19IGAccelEventMachine"
EVENT_MERGE = "__ZN24IOAccelEventMachineFast210mergeEventEP12IOAccelEventS1_"
GC_OBJECT_RELEASE = "__ZNK10IGGCObject7releaseEv"
GC_OBJECT_RELEASE_UNCHECKED = "__ZNK10IGGCObject14releaseNoCheckEv"
GC_ADD = "__ZN18IGGarbageCollector3addEP14IGGCQueueEntry"
GC_COLLECT = "__ZN18IGGarbageCollector7collectEv"
GC_FORCE_COLLECT = "__ZN18IGGarbageCollector12forceCollectEv"
GC_DRAIN = "__ZN18IGGarbageCollector5drainEv"
CONTEXT_CHECK = "__ZNK17IGHardwareContext5checkEv"
CONTEXT_VTABLE = "__ZTV17IGHardwareContext"
SCHEDULER4_CONTEXT_IDLE = "__ZNK12IGScheduler413isContextIdleEPK17IGHardwareContext"
GUC_KMD_CONTEXT_IDLE = "__ZN13IGHardwareGuC16isKmdContextIdleERK21SGfxContextDescriptor"
SHARED_BUFFER_CPU_ADDRESS = "__ZNK20IGSharedMappedBuffer17getVirtualAddressEv"
MAPPED_BUFFER_GPU_ADDRESS = "__ZNK14IGMappedBuffer20getGPUVirtualAddressEv"
MEMORY_MAP_VTABLE = "__ZTV16IGAccelMemoryMap"
MEMORY_MAP_GPU_ADDRESS = "__ZN16IOAccelMemoryMap20getGPUVirtualAddressEv"
MAPPED_BUFFER_INIT = "__ZN14IGMappedBuffer15initWithOptionsEP11IGAccelTaskmbj"
SYS_MEMORY_FACTORY = "__ZN16IOAccelSysMemory11withOptionsEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2P16IOAccelResource2jy"
PREPARE_MAPPING = "__ZN22IOGraphicsAccelerator220freeToPrepareMappingEP16IOAccelMemoryMap"
POPULATE_ACCEL_CONFIG = "__ZN16IntelAccelerator19populateAccelConfigEP13IOAccelConfig"
MAPPED_BUFFER_MAPPING_OPTIONS = "__ZNK14IGMappedBuffer17getMappingOptionsEv"
MAPPED_BUFFER_FREE = "__ZN14IGMappedBuffer4freeEv"
SHARED_BUFFER_FREE = "__ZN20IGSharedMappedBuffer4freeEv"
SHARED_BUFFER_UNLOCK = "__ZN20IGSharedMappedBuffer18unlockForCPUAccessEv"
MAPPED_BUFFER_VTABLE = "__ZTV14IGMappedBuffer"
SHARED_BUFFER_VTABLE = "__ZTV20IGSharedMappedBuffer"
MEMORY_MAP_COMPLETE = "__ZN16IOAccelMemoryMap8completeEv"
MEMORY_MAP_FINISH_EVENT = "__ZN16IOAccelMemoryMap11finishEventEv"
SYS_MEMORY_UNLOCK = "__ZN16IOAccelSysMemory18unlockForCPUAccessEP4task"
SHARED_BUFFER_CLONE = "__ZN20IGSharedMappedBuffer11cloneInTaskEP11IGAccelTask"
SHARED_BUFFER_FACTORY = "__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj"
SCHEDULER4_BIND = "__ZN12IGScheduler44bindE10IGHwCsTypeih"
SCHEDULER4_UNBIND = "__ZN12IGScheduler46unbindEP17IGHardwareContext"
SCHEDULER4_PUSH = "__ZN12IGScheduler44pushEP17IGHardwareContextjjbb"
RING_SUBMIT_TO_RING = "__ZN20IGHardwareRingBuffer12submitToRingEv"
RING_SUBMIT_FAILURE = RING_SUBMIT_TO_RING + ".cold.1"
GUC_SUBMIT_WORK_ITEM = "__ZN13IGHardwareGuC14submitWorkItemEjRK21SGfxContextDescriptor10IGHwCsTypejjj"
FIFO_FACTORY = "__ZN18IGAccelFIFOChannel11withOptionsEP22IOGraphicsAccelerator2P20IGHardwareRingBuffer"
FIFO_INIT = "__ZN18IGAccelFIFOChannel15initWithOptionsEP22IOGraphicsAccelerator2P20IGHardwareRingBuffer"
FIFO_FREE = "__ZN18IGAccelFIFOChannel4freeEv"
FIFO_SUBMIT_COMMANDS = "__ZN18IGAccelFIFOChannel18submitRingCommandsEPjjj"
FIFO_SUBMIT_BUFFER = "__ZN18IGAccelFIFOChannel12submitBufferEP24IOAccelCommandDescriptor"
ACCEL_SUBMIT_SYNC = "__ZN16IntelAccelerator16submitSyncEventsEbP11IGAccelTask10IGHwCsTypeb"
ACCEL_SUBMIT_MAIN = "__ZN16IntelAccelerator21submitMainRingCommandEPjm"
GET_DEFAULT_RESET = "__ZN16IntelAccelerator20getDefaultResetValueEj"
TRACE_DISABLE = "__ZN25IGAccelTraceStreamManager17disableCollectionE27TraceStreamCollectionChange"
TRACE_SHUTDOWN = "__ZN25IGAccelTraceStreamManager8shutdownEv"
UNREGISTER_SYSCTL = "__ZN16IntelAccelerator16unregisterSysctlEv"
INIT_HARDWARE_STATUS_MEMORY = "__ZN16IntelAccelerator28initHardwareStatusPageMemoryEv"
INIT_HARDWARE_STATUS_REGISTERS = "__ZN16IntelAccelerator31initHardwareStatusPageRegistersEv"
INIT_MODE_REGISTERS = "__ZN16IntelAccelerator17initModeRegistersEv"
SAFE_FORCE_WAKE = "__ZN16IntelAccelerator13SafeForceWakeEbj"
SAFE_FORCE_WAKE_BOOL = "__ZN16IntelAccelerator13SafeForceWakeEb"
BRIDGE_ENABLE = "__ZN17IGInterruptBridge6enableEv"
BRIDGE_DISABLE = "__ZN17IGInterruptBridge7disableEv"
BRIDGE_FILTER = "__ZN17IGInterruptBridge22interruptFilterHandlerEP28IOFilterInterruptEventSource"
BRIDGE_READ = "__ZN17IGInterruptBridge22readAndClearInterruptsER8IGBitSetILm46EE"
BRIDGE_ENABLE_INTERRUPTS = "__ZN17IGInterruptBridge16enableInterruptsEv"
BRIDGE_DISABLE_INTERRUPTS = "__ZN17IGInterruptBridge17disableInterruptsEv"
BRIDGE_READ_RCS = "__ZN17IGInterruptBridge25readAndClearRCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_CCS = "__ZN17IGInterruptBridge25readAndClearCCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_BCS = "__ZN17IGInterruptBridge25readAndClearBCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_GUC = "__ZN17IGInterruptBridge25readAndClearGuCInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_VCS = "__ZN17IGInterruptBridge25readAndClearVCSInterruptsER8IGBitSetILm46EEj"
BRIDGE_READ_VECS = "__ZN17IGInterruptBridge26readAndClearVECSInterruptsER8IGBitSetILm46EEj"
EVENT_INIT = "__ZN24IOAccelEventMachineFast29initEventEP12IOAccelEvent"
SCHEDULER_ENABLE = "__ZN12IGScheduler416enableInterruptsEv"
SCHEDULER_DISABLE = "__ZN12IGScheduler417disableInterruptsEv"
SCHEDULER_ERROR_ENABLE = "__ZN12IGScheduler421enableErrorInterruptsEv"
SCHEDULER_ERROR_DISABLE = "__ZN12IGScheduler422disableErrorInterruptsEv"
STREAMER_ERROR_ENABLE = "__ZN26IGHardwareCommandStreamer420enableErrorInterruptEv"
STREAMER_ERROR_DISABLE = "__ZN26IGHardwareCommandStreamer421disableErrorInterruptEv"
PCI_CONFIGURE_INTERRUPTS = "__ZN11IOPCIDevice19configureInterruptsEjjjj"
SCHEDULER_INIT_FIRMWARE = "__ZN11IGScheduler12initFirmwareEv"
SCHEDULER4_SYSTEM_SLEEP = "__ZN12IGScheduler415systemWillSleepEv"
SCHEDULER4_SYSTEM_WAKE = "__ZN12IGScheduler413systemDidWakeEv"
BRIDGE_SYSTEM_SLEEP = "__ZN17IGInterruptBridge15systemWillSleepEv"
BRIDGE_SYSTEM_WAKE = "__ZN17IGInterruptBridge13systemDidWakeEv"
SET_POWER_STATE = "__ZN16IntelAccelerator13setPowerStateEmP9IOService"
SCHEDULER4_VTABLE = "__ZTV12IGScheduler4"
SCHEDULER5_VTABLE = "__ZTV12IGScheduler5"
SCHEDULER4_INIT = "__ZN12IGScheduler419initWithAcceleratorEP22IOGraphicsAccelerator2"
SCHEDULER4_LOAD_FIRMWARE = "__ZN12IGScheduler412loadFirmwareEv"
SCHEDULER4_IS_GPU_IDLE = "__ZNK12IGScheduler49isGpuIdleEv"
SCHEDULER5_IS_GPU_IDLE = "__ZNK12IGScheduler59isGpuIdleEv"
SCHEDULER_BASE_INIT = "__ZN11IGScheduler15initWithOptionsEjyP22IOGraphicsAccelerator2"
COMMAND_STREAMER_FACTORY = "__ZN26IGHardwareCommandStreamer423hardwareCommandStreamerEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler410IGHwCsType"
COMMAND_STREAMER_INIT = "__ZN26IGHardwareCommandStreamer44initEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler410IGHwCsType"
COMMAND_STREAMER_REGISTER = "__ZN26IGHardwareCommandStreamer421registerForInterruptsEv"
REQUEST_ENABLE_CALLBACK = "__ZN17IGInterruptBridge21requestEnableCallbackEP8OSObjectPFvS1_zE"
GUC_INIT_INTERRUPTS = "__ZN13IGHardwareGuC14initInterruptsEv"
GUC_REGISTER_INTERRUPTS = "__ZN13IGHardwareGuC21registerForInterruptsEv"
BRIDGE_REGISTER_TYPE = "__ZN17IGInterruptBridge24registerForInterruptTypeEjPFvP8OSObjectPvES1_S2_PS2_"
GUC_WITH_OPTIONS = "__ZN13IGHardwareGuC11withOptionsEP16IntelAccelerator"
GUC_INIT_WITH_OPTIONS = "__ZN13IGHardwareGuC15initWithOptionsEP16IntelAccelerator"
GUC_FREE = "__ZN13IGHardwareGuC4freeEv"
GUC_INIT_SCHED_CONTROL = "__ZN13IGHardwareGuC16initSchedControlEv"
GUC_SETUP_CONTEXT_POOL = "__ZN13IGHardwareGuC16setupContextPoolEi"
GUC_SETUP_LOG_BUFFERS = "__ZN13IGHardwareGuC15setupLogBuffersEjiii"
GUC_SETUP_ADDITIONAL = "__ZN13IGHardwareGuC26setupAdditionalDataStructsEv"
GUC_INIT_WORK_HISTORY = "__ZN13IGHardwareGuC19initWorkItemHistoryEj"
GUC_INIT_DOORBELLS = "__ZN13IGHardwareGuC13initDoorbellsEv"
GUC_READ_DOORBELLS = "__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv"
CTB_WITH_OPTIONS = "__ZN21IGHardwareGuCCTBuffer11withOptionsEP22IOGraphicsAccelerator2"
CTB_INIT = "__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2"
CTB_FREE = "__ZN21IGHardwareGuCCTBuffer4freeEv"
SET_ASYNC_SLICE_COUNT = "__ZN16IntelAccelerator18setAsyncSliceCountE13IGSliceConfig"
DPSM_IDLE_TIMER = "__ZN16IntelAccelerator13dpsmIdleTimerEv"
INIT_LOCAL_CALLBACKS = "__ZN16IntelAccelerator24initLocalCallbackSupportEv"
ENABLE_COARSE_POWER_GATING = "__ZL24_enableCoarsePowerGatingv"
DPSM_NOTIFY = "__ZL11_dpsmNotifyPj"
LOCAL_SAFE_FORCE_WAKE = "__ZL14_SafeForceWakebj"
LOCAL_PAVP_CONTROL = "__ZL19_PAVPSessionControl27PAVPSessionControlCommand_tPv"
LOCAL_MEDIA_LOAD = "__ZL16_mediaKernelLoadb"
LOCAL_MEDIA_PREPARE = "__ZL19_mediaPrepareEncodePv"
LOCAL_CLIENT_NOTIFY = "__ZL13_clientNotify15IntelClientID_tb"
LOCAL_PM_NOTIFY = "__ZL9_pmNotifyjjPyPj"
LOCAL_GUC_WILL_LOAD = "__ZL17_accelWillLoadGuCyPv"
LOCAL_GUC_FAILED = "__ZL21_accelFailedToLoadGuCv"
LOCAL_GUC_DID_LOAD = "__ZL16_accelDidLoadGuCv"
GUC_LOAD_BINARY = "__ZN13IGHardwareGuC13loadGuCBinaryEv"
GUC_REGISTER_CTB = "__ZN13IGHardwareGuC31registerCommandTransportBuffersEv"
GUC_DEREGISTER_CTB = "__ZN13IGHardwareGuC33deregisterCommandTransportBuffersEv"
GUC_MMIO_ACTION = "__ZN13IGHardwareGuC19mmioHostToGuCActionEPKjjiPj"
CREATE_UK_CONTEXT = "__ZN13IGHardwareGuC15createUkContextEy25UK_GEN11_CONTEXT_PRIORITY"
MAPPED_WITH_OPTIONS = "__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj"
MAPPED_INIT = "__ZN20IGSharedMappedBuffer15initWithOptionsEP11IGAccelTaskmjj"
MAPPED_GET_MEMORY = "__ZNK14IGMappedBuffer9getMemoryEv"
SYS_MEMORY_PHYSICAL = "__ZN16IGAccelSysMemory18getPhysicalSegmentEyPy"
TRANSFER_OWNERSHIP = "__ZN16IntelAccelerator17transferOwnershipEPK20IGSharedMappedBufferi"
GLOBAL_MAP_RANGE = "__ZN25IGHardwareGlobalPageTable8mapRangeERK14IGAddressRangeyy"
GLOBAL_MAP_ROTATED = "__ZN25IGHardwareGlobalPageTable15mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory"
GLOBAL_UNMAP_RANGE = "__ZN25IGHardwareGlobalPageTable10unmapRangeERK14IGAddressRange"
GLOBAL_MAP_DUMMY = "__ZN25IGHardwareGlobalPageTable13mapRangeDummyERK14IGAddressRangey"
FENCE_ALLOCATE = "__ZN16IGFenceAllocator8allocateERK14IGAddressRangem19GFX3DSTATE_TILEMODE"
FENCE_INIT = "__ZN7IGFence15initWithOptionsEP16IGFenceAllocatormRK14IGAddressRangem19GFX3DSTATE_TILEMODE"
FENCE_FREE = "__ZN7IGFence4freeEv"
RESOURCE_ADD_APERTURE = "__ZN15IGAccelResource13addToApertureEv"
DISPLAY_ALLOC_SCANOUT = "__ZN18IGAccelDisplayPipe21allocateScanoutMemoryEP19IntelScaledModeDataj"
ACCELERATOR_VTABLE = "__ZTV16IntelAccelerator"
NEW_MEMORY_MANAGER = "__ZN16IntelAccelerator16newMemoryManagerEv"
TGL_MEMORY_METACLASS = "__ZN21IntelTGLMemoryManager9metaClassE"
TGL_MEMORY_VTABLE = "__ZTV21IntelTGLMemoryManager"
MEMORY_MANAGER_INIT = "__ZN15IGMemoryManager4initEP16IntelAcceleratorRK18IntelSharedMemInfoRK14_stolenMemInfo"
TGL_DETECT_EDRAM = "__ZN21IntelTGLMemoryManager11detectEDRAMEv"
BLIT3D_BOUNDS_START = "__ZN23IGHardwareBlit2DContext10initializeEv"
BLIT3D_BOUNDS_END = "__ZN21IGAccelDisplayMachine9MetaClassC1Ev"
BLIT3D_GLOBAL_INIT = "__GLOBAL__sub_I_IGHardwareContext.cpp"
BLIT3D_SCRATCH_ANCHOR = bytes.fromhex(
    "48 8d 05 19 31 03 00 48 8b 00 48 89 05 df d6 0c 00")
# Error 0x215 clears the native start result and jumps directly to the final
# result/stack-check block, bypassing Tahoe's common virtual-stop cleanup.
DPSM_START_FAILURE_ANCHOR = bytes.fromhex(
    "be 15 02 00 00 45 31 f6 e9 41 ff ff ff")
ASYNC_SLICE_MMIO_ANCHOR = bytes.fromhex(
    "49 8b 86 40 12 00 00 89 98 04 a2 00 00")
HWS_ENGINE_MMIO_ANCHOR = bytes.fromhex(
    "49 8b 8e 40 12 00 00 42 89 04 21")
HWS_GLOBAL_MMIO_ANCHOR = bytes.fromhex(
    "49 8b 8e 40 12 00 00 89 81 80 80 01 00")
FENCE_INIT_MMIO_ANCHOR = bytes.fromhex(
    "48 8b 8f 40 12 00 00 89 1c 01 48 8b 45 c8 44 89 2c 01")
FENCE_FREE_MMIO_ANCHOR = bytes.fromhex(
    "48 8b 8f 40 12 00 00 89 1c 01 46 89 2c 31")
FENCE_RESOURCE_NULL_UNWIND = bytes.fromhex(
    "49 89 86 28 02 00 00 48 85 c0 74 60")
MEMORY_EDRAM_ZERO_STATE = bytes.fromhex("66 41 89 46 20")
EDRAM_CAPABILITY_MMIO_READ = bytes.fromhex(
    "49 8b 46 18 8b 98 10 00 12 00")
EDRAM_CONTROL_MMIO_WRITES = bytes.fromhex(
    "49 8b 46 18 bb 00 00 00 b0 89 98 28 81 13 00 "
    "c7 80 24 81 13 00 12 10 59 80")
EDRAM_CONFIRM_MMIO_READ = bytes.fromhex(
    "49 8b 46 18 8b 80 10 59 14 00")
DPSM_SCHEDULER_IDLE_SLOT = bytes.fromhex(
    "48 8b 07 ff 90 60 01 00 00")
DPSM_NOTIFY_SLOT = bytes.fromhex(
    "48 8b 83 e8 0d 00 00 ff 50 38")
DPSM_COARSE_POWER_SLOT = bytes.fromhex(
    "49 8b 85 e8 0d 00 00 ff 10")
DPSM_NOTIFY_BODY = bytes.fromhex("55 48 89 e5 31 c0 5d c3")
VOID_NOOP_BODY = bytes.fromhex("55 48 89 e5 5d c3")
ZERO_NOOP_BODY = bytes.fromhex("55 48 89 e5 31 c0 5d c3")
UNSUPPORTED_NOOP_BODY = bytes.fromhex(
    "55 48 89 e5 b8 c7 02 00 e0 5d c3")


def direct_branch_candidates(image, start, end, target, external_offsets):
    """Bounded byte candidates, not an x86 instruction/reachability decoder.

    External relocations can carry zero placeholders which accidentally look
    like a local branch to the next function. Never treat their disk addend as
    the final runtime displacement. Whole-body/anchor checks remain required.
    """
    assert 0 <= start <= end <= len(image)
    found = []
    for candidate in range(start, end - 4):
        if image[candidate] not in (0xe8, 0xe9):
            continue
        if any(offset in external_offsets for offset in range(candidate + 1, candidate + 5)):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 1)[0]
        if candidate + 5 + displacement == target:
            found.append(candidate)
    return found


def direct_branch_candidate_contract():
    placeholder = bytes.fromhex("e9 00 00 00 00")
    assert direct_branch_candidates(placeholder, 0, 5, 5, set()) == [0]
    for relocated in range(1, 5):
        assert direct_branch_candidates(placeholder, 0, 5, 5, {relocated}) == []
    real = bytes.fromhex("e8 fb ff ff ff")
    assert direct_branch_candidates(real, 0, 5, 0, set()) == [0]
    assert direct_branch_candidates(real, 0, 4, 0, set()) == []
    assert direct_branch_candidates(b"", 0, 0, 0, set()) == []
    assert direct_branch_candidates(real, 0, 5, 0, {5}) == [0]


def macho_inventory(path):
    image = pathlib.Path(path).read_bytes()
    header = struct.unpack_from("<8I", image)
    if header[0] != 0xFEEDFACF:
        raise AssertionError(f"{path}: not a little-endian Mach-O 64 image")

    symtab = None
    dysymtab = None
    offset = 32
    for _ in range(header[4]):
        command, size = struct.unpack_from("<2I", image, offset)
        if command == 0x2:
            symtab = struct.unpack_from("<6I", image, offset)[2:]
        elif command == 0xB:
            dysymtab = struct.unpack_from("<20I", image, offset)
        offset += size
    if not symtab or not dysymtab:
        raise AssertionError(f"{path}: missing Mach-O symbol/relocation tables")

    symbol_offset, symbol_count, string_offset, _ = symtab
    names = []
    values = []
    for index in range(symbol_count):
        string_index, _, _, _, value = struct.unpack_from(
            "<IBBHQ", image, symbol_offset + index * 16)
        if string_index:
            end = image.index(0, string_offset + string_index)
            name = image[string_offset + string_index:end].decode()
        else:
            name = ""
        names.append(name)
        values.append(value)

    def value(name):
        matches = [values[i] for i, candidate in enumerate(names)
                   if candidate == name and values[i]]
        if len(matches) != 1:
            raise AssertionError(f"{path}: expected one defined {name}")
        return matches[0]

    wanted = {ENABLE, DISABLE}
    wanted_indexes = {index: name for index, name in enumerate(names)
                      if name in wanted}
    relocations = {}
    panic_relocations = set()
    inherited_event_imports = {}
    external_offset, external_count = dysymtab[16], dysymtab[17]
    # These imports distinguish the periodic collection mutex from bridge
    # descriptor spin locks. They do not certify dynamic callback lifetime.
    stamp_irq_imports = {
        0x12618: "_IOMalloc",
        0x12631: "_memset",
        0x79a4d: "_IOMalloc",
        0x79a89: "_IOFree",
        0x78580: "_IOLockLock",
        0x786b3: "_IOLockUnlock",
        0x78594: "__ZN22IOGraphicsAccelerator29lock_busyEv",
        0x786a7: "__ZN22IOGraphicsAccelerator211unlock_busyEv",
        0x785f7: "__ZN14IOAccelShared214lookupResourceEjPPv",
        0x2c82f: "__ZN25IOAccelCommandBufferPool212submitBufferEv",
        0x74519: "_IOFree",
        0x74535: "_IOFree",
        0x74317: "__ZN16IOAccelResource218getStorageResourceEv",
        0x799bf: "__ZN16IOAccelResource210checkDirtyEv",
        0x78a10: "_IOLockLock",
        0x78e3d: "_IOLockUnlock",
        0x78a20: "__ZN22IOGraphicsAccelerator29lock_busyEv",
        0x78e31: "__ZN22IOGraphicsAccelerator211unlock_busyEv",
        0x78a82: "__ZN14IOAccelShared214lookupResourceEjPPv",
        0x15ec9: "__ZN16IOSimpleReporter14incrementValueEyx",
        0x73c16: "_IOMalloc",
        0x73ed3: "_IOFree",
        0x73eef: "_IOFree",
        0x73f0b: "_IOFree",
        0x73f2b: "_IOFree",
        0x73f47: "_IOFree",
        0x757d1: "_IOMalloc",
        0x7580d: "_IOFree",
        0x2cad9: "__ZN25IOAccelCommandBufferPool212submitBufferEv",
        0x11e16: "_IOFree",
        0xcdc7: "_memset", 0xce0b: "_memset", 0xce5d: "_memset", 0xcea0: "_memset",
        0xb895: "_memset",
        0x2810c: "__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass",
        0x28164: "__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass",
        0x28191: "__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass",
        0x281e0: "__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass",
        0x2813d: "___memcpy_chk",
        0x281b5: "__ZN15IORegistryEntry8fromPathEPKcPK15IORegistryPlanePcPiPS_",
        0x281fe: "__ZN8OSNumber10withNumberEPKcj",
        0x41e76: "___stack_chk_fail",
        0x90a80: "_panic",
        0x41b06: "_assert_wait_timeout", 0x41b0d: "_thread_block",
        0x41b54: "_mach_absolute_time", 0x41b89: "_mach_absolute_time",
        0xe27e: "_IOMalloc", 0xe297: "_memset",
        0xa720: "__ZN8OSObjectC2EPK11OSMetaClass",
        0xa7c3: "__ZN8OSObjectnwEm", 0xa7d8: "__ZN8OSObjectC2EPK11OSMetaClass",
        0xa7ea: "__ZNK11OSMetaClass19instanceConstructedEv",
        0xa73c: "__ZN8OSObjectD2Ev", 0xa746: "__ZN8OSObjectD2Ev",
        0xa754: "__ZN8OSObjectD2Ev", 0xa767: "__ZN8OSObjectdlEPvm",
        0xbab2: "__ZN22IOInterruptEventSource20interruptEventSourceEP8OSObjectPFvS1_PS_iEP9IOServicei",
        0xbaca: "__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E",
        0xb209: "__ZN24IOBufferMemoryDescriptor17inTaskWithOptionsEP4taskjmm",
        0xb2dd: "__ZN18IOMemoryDescriptor19createMappingInTaskEP4taskyjyy",
        0xb30b: "_OSAddAtomic64",
        0xb0f9: "_OSAddAtomic64",
        0xb825: "_OSAddAtomic64", 0xbb67: "_OSAddAtomic64",
        0x80dfc: "__ZNK18IOAccelDisplayPipe15getEventMachineEv",
        0x80e16: "__ZNK18IOAccelDisplayPipe15getEventMachineEv",
        0x80eb0: "__ZNK18IOAccelDisplayPipe15getEventMachineEv",
        0x5657a: "_IOLockAlloc", 0x5663e: "_IOLockFree",
        0x37ce4: "__ZN10IOWorkLoop8workLoopEv", 0x37d58: "_IOMalloc",
        0x27ad5: "_PE_parse_boot_argn",
        0x24767: "__ZN22IOGraphicsAccelerator211unlock_busyEv",
        0x24773: "_IOLockUnlock",
        0x2464a: "__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E",
        0x281b5: "__ZN15IORegistryEntry8fromPathEPKcPK15IORegistryPlanePcPiPS_",
        0x281fe: "__ZN8OSNumber10withNumberEPKcj",
        0x28191: "__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass",
        0x37d08: "__ZN22IOInterruptEventSource20interruptEventSourceEP8OSObjectPFvS1_PS_iEP9IOServicei",
        0x56549: "__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E",
        0x5652e: "__ZN5OSSet12withCapacityEj", 0x564cb: "_memset",
        0x15ccb: "__ZN22IOInterruptEventSource20interruptEventSourceEP8OSObjectPFvS1_PS_iEP9IOServicei",
        0x2acf8: "__ZN22IOGraphicsAccelerator219signalStampsUpdatedEv",
        0x2acfd: "_mach_absolute_time",
        0x2ac5b: "_kernel_debug", 0x2ad4b: "___stack_chk_fail",
        0x5669d: "_IOLockLock", 0x5671f: "_IOLockUnlock",
        0x56855: "_IOLockLock", 0x56892: "_IOLockUnlock",
        0x568b1: "_IOLockLock", 0x56910: "_IOLockUnlock",
        0x56929: "_IOLockLock", 0x56974: "_IOLockUnlock",
        0x566a9: "__ZN20OSCollectionIterator14withCollectionEPK12OSCollection",
        0x4acc2: "_lck_spin_lock", 0x4ad71: "_lck_spin_unlock",
        0x4adce: "_lck_spin_lock", 0x4ae7d: "_lck_spin_unlock",
    }
    observed_stamp_irq_imports = {address: [] for address in stamp_irq_imports}
    event_stop_imports = {
        0xca608: "__ZNK8OSObject14getRetainCountEv",
        0xca610: "__ZNK8OSObject6retainEv",
        0xca638: "__ZNK8OSObject12taggedRetainEPKv",
        0xdb7e0: "__ZN24IOAccelSharedUserClient25startEP9IOService",
        0xc81c8: "__ZTV24IOAccelSharedUserClient2",
        0xd1920: "__ZN22IOGraphicsAccelerator212createSharedEP4task",
        0xd1a78: "__ZN22IOGraphicsAccelerator29newSharedEv",
        0xc8118: "__ZTV11IOAccelTask",
        0xd9540: "__ZN16IOAccelResource213sharedReleaseEP14IOAccelShared2",
        0xd9570: "__ZN16IOAccelResource212addToChannelEP15IOAccelChannel2j",
        0xd9578: "__ZN16IOAccelResource217removeFromChannelEP15IOAccelChannel2",
        0xc81b8: "__ZTV24IOAccelEventMachineFast2",
        0xcebd0: "__ZN24IOAccelEventMachineFast219mergeEventExcludingEP12IOAccelEventS1_i",
        0xcebf8: "__ZN24IOAccelEventMachineFast224writeEventBarrierCommandEP17IOAccelEventQueueP12IOAccelEventP17vendevtBarrierReci",
        0xc81a8: "__ZTV22IOGraphicsAccelerator2",
        0xd65c0: "__ZN15IOAccelChannel213setEventStampEP12IOAccelEvent",
        0xc8240: "_real_ncpus",  # Pool count is a linked kernel datum, not zero.
        0xd19b0: "__ZN22IOGraphicsAccelerator223freeWaitToPrepareVidMapEP16IOAccelMemoryMapbb",
        0xd19d8: "__ZN22IOGraphicsAccelerator223freeWaitToPrepareSysMapEP16IOAccelMemoryMapb",
        0xcd040: "__ZN16IOAccelMemoryMap7prepareEv",
        0xd1b20: "__ZN22IOGraphicsAccelerator218createIODMACommandEv",
        0xc8140: "__ZTV16IOAccelSysMemory",
        0xcd778: "__ZN16IOAccelSysMemory4freeEv",
        0xc8130: "__ZTV16IOAccelMemoryMap",
        0xd9550: "__ZN16IOAccelResource27prepareEv",
        0xc8138: "__ZTV16IOAccelResource2",
        0xcebd8: "__ZN24IOAccelEventMachineFast213setEventStampEiP12IOAccelEvent",
        0xcebe0: "__ZN24IOAccelEventMachineFast214incrementStampEi",
        0xcebe8: "__ZN24IOAccelEventMachineFast217writeStampCommandEiP17IOAccelEventQueueP17vendevtCommandRec",
        0xc81c0: "__ZTV24IOAccelLegacyDisplayPipe",
        0xceb60: "__ZN24IOAccelEventMachineFast215finishAllStampsEv",
        0xcec70: "__ZN20IOAccelEventMachine24stopEv",
    }
    for table in ("__ZTV10IGPagePool", "__ZTV31IGHardwarePerProcessPageTable32",
                  "__ZTV31IGHardwarePerProcessPageTable64", "__ZTV25IGHardwareGlobalPageTable"):
        for slot, method in ((0x20, "__ZNK8OSObject6retainEv"),
                             (0x28, "__ZNK8OSObject7releaseEv"),
                             (0x48, "__ZNK8OSObject12taggedRetainEPKv"),
                             (0x50, "__ZNK8OSObject13taggedReleaseEPKv"),
                             (0x58, "__ZNK8OSObject13taggedReleaseEPKvi")):
            event_stop_imports[value(table) + 16 + slot] = method
    observed_event_stop_imports = {address: [] for address in event_stop_imports}
    external_relocation_offsets = set()
    for index in range(external_count):
        address, bits = struct.unpack_from(
            "<iI", image, external_offset + index * 8)
        symbol_index = bits & 0xFFFFFF
        external_relocation_offsets.add(address)
        if address in observed_event_stop_imports:
            observed_event_stop_imports[address].append((names[symbol_index], bits >> 24))
        if address in observed_stamp_irq_imports:
            observed_stamp_irq_imports[address].append((names[symbol_index], bits >> 24))
        if names[symbol_index] in (
                "__ZN15IOAccelChannel219mergeEventExcludingEP12IOAccelEventS1_",
                "__ZN15IOAccelChannel213setEventStampEP12IOAccelEvent",
                "__ZN15IOAccelChannel214incrementStampEv",
                "__ZN22IOGraphicsAccelerator211scrubEventsEv",
                "__ZN14IOAccelShared211scrubEventsEv",
                "__ZN16IOAccelResource211scrubEventsEv",
                "__ZN24IOAccelEventMachineFast210scrubEventEP12IOAccelEvent"):
            if address in inherited_event_imports:
                raise AssertionError(f"{path}: duplicate channel method relocation")
            inherited_event_imports[address] = (names[symbol_index], bits >> 24)
        if names[symbol_index] == "_panic":
            if ((bits >> 24) & 1, (bits >> 25) & 3,
                    (bits >> 27) & 1, (bits >> 28) & 0xF) != (1, 2, 1, 2):
                raise AssertionError(f"{path}: incompatible panic relocation")
            panic_relocations.add(address)
        if symbol_index in wanted_indexes:
            name = wanted_indexes[symbol_index]
            if name in relocations:
                raise AssertionError(f"{path}: duplicate {name} relocation")
            # X86_64_RELOC_BRANCH, external, PC-relative, 32-bit displacement.
            if ((bits >> 24) & 1, (bits >> 25) & 3,
                    (bits >> 27) & 1, (bits >> 28) & 0xF) != (1, 2, 1, 2):
                raise AssertionError(f"{path}: incompatible {name} relocation")
            relocations[name] = address

    for address, name in stamp_irq_imports.items():
        opcode = 0xe9 if address in (0xb825, 0xa73c, 0xa746, 0xa767) or (name in ("_IOLockUnlock", "_lck_spin_unlock") and address not in (0x24773, 0x78e3d, 0x786b3)) else 0xe8
        if observed_stamp_irq_imports[address] != [(name, 0x2d)] or image[address - 1] != opcode:
            raise AssertionError(f"{path}: changed stamp IRQ imported call at {address:#x}")

    for address, name in event_stop_imports.items():
        assert observed_event_stop_imports[address] == [(name, 0x0e)], \
            f"{path}: changed inherited event teardown virtual import"
    pool_table = value("__ZTV10IGPagePool")
    for slot, method in ((0, "__ZN10IGPagePoolD1Ev"),
                         (8, "__ZN10IGPagePoolD0Ev"),
                         (0x90, "__ZN10IGPagePool4freeEv")):
        assert struct.unpack_from("<Q", image, pool_table + 16 + slot)[0] == value(method), f"{path}: changed pool deletion/free virtual target"
    for slot, address in ((0x158, 0xceb60), (0x268, 0xcec70)):
        assert value(EVENT_MACHINE_VTABLE) + 16 + slot == address, \
            f"{path}: inherited event teardown import moved outside effective vtable"

    def next_symbol(address):
        following = sorted(candidate for candidate in values
                           if candidate > address)
        if not following:
            raise AssertionError(f"{path}: no symbol after {address:#x}")
        return following[0]

    def direct_branches(owner, target):
        owner_start = value(owner)
        owner_end = next_symbol(owner_start)
        target_start = value(target)
        return direct_branch_candidates(image, owner_start, owner_end,
                                        target_start, external_relocation_offsets)

    for name, (length, digest) in STAMP_IRQ_NATIVE.items():
        start = value(name)
        assert next_symbol(start) - start == length, f"{path}: changed stamp IRQ body boundary: {name}"
        assert hashlib.sha256(image[start:start + length]).hexdigest() == digest, f"{path}: changed stamp IRQ body: {name}"
    # Duplicate template/private names require the selected reviewed address;
    # never silently choose another instantiation with the same symbol name.
    for name, start, length, digest in (
            ("__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm", 0x757b2, 0x78, "d0eadd8fc2d8b9227e151fa3b7f5e650f0be03ffd879463ac349f75217c6c440"),
            ("__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", 0x7582a, 0x172, "5dde422ed4edf1c957284fb9856877845d570a8e04df2d4d8f27225f1b7b245a"),
            ("__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm", 0x79a2e, 0x78, "d0eadd8fc2d8b9227e151fa3b7f5e650f0be03ffd879463ac349f75217c6c440"),
            ("__ZL20AddDstResourceEventsR18wait_update_eventsP15IGAccelResourceb", 0x79aa6, 0x176, "c7d57dca9a6af01f663c01137cbc42b87d024a00e2fcd3926bf5c3752e0d53a8")):
        assert any(candidate == name and address == start for candidate, address in zip(names, values)), f"{path}: missing selected CCS helper: {name}"
        assert next_symbol(start) == start + length, f"{path}: changed selected CCS helper boundary: {name}"
        assert hashlib.sha256(image[start:start + length]).hexdigest() == digest, f"{path}: changed selected CCS helper: {name}"
    assert image[0x79ac1:0x79ac5] == bytes.fromhex("84 d2 75 3c"), f"{path}: changed user-client wait omission option (not allocation status)"
    assert image[0x14518:0x1451e] == bytes.fromhex("ff 90 30 01 00 00"), f"{path}: changed releaseRange unmap virtual"
    assert direct_branches("__ZN19IGHardwarePageTable12releaseRangeERK14IGAddressRange", "__ZN16IntelAccelerator27flushHardwareAfterGttUpdateEv") == [0x14522], f"{path}: changed post-unmap deferred-flush edge"
    assert image[0x14573:0x14575] == bytes.fromhex("b0 01"), f"{path}: changed unconditional releaseRange success"
    assert image[0xb35e:0xb366] == bytes.fromhex("4c 89 73 d8 4c 89 6b e0"), f"{path}: changed raw descriptor pool/block owner stores"
    assert image[0xf3da:0xf3dd] == bytes.fromhex("ff 51 28"), f"{path}: changed manager pool owner release virtual"
    assert struct.unpack_from("<Q", image, value("__ZTV31IGHardwarePerProcessPageTable32") + 16 + 0x90)[0] == value("__ZN31IGHardwarePerProcessPageTable324freeEv"), f"{path}: changed 32-bit page-table free dispatch"
    assert direct_branches("__ZN31IGHardwarePerProcessPageTable324freeEv", "__ZN10IGPagePool14PageDescriptor7releaseEv") == [0x11df9, 0x11e1e], f"{path}: changed 32-bit leaf/parent descriptor destruction"
    assert struct.unpack_from("<Q", image, value("__ZTV31IGHardwarePerProcessPageTable64") + 16 + 0x90)[0] == value("__ZN31IGHardwarePerProcessPageTable644freeEv"), f"{path}: changed 64-bit page-table free dispatch"
    assert direct_branches("__ZN11IGAccelTask4freeEv", "__ZN11IGAccelTask27releaseManagedPageTableListEv") == [0x7ddd], f"{path}: changed task final list cleanup"
    assert image[0x7dbd:0x7dcf] == bytes.fromhex("48 8b bb 60 02 00 00 48 85 ff 74 06 48 8b 07 ff 50 28"), f"{path}: changed private page-table release before list cleanup"
    assert direct_branches("__ZN15IGMemoryManager27releaseFromPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap", "__ZN19IGHardwarePageTable12releaseRangeERK14IGAddressRange") == [0xf74e], f"{path}: changed task fan-out release edge"
    assert image[0xf753:0xf75f] == bytes.fromhex("41 20 c7 48 8b 5b 08 48 85 db 75 e9"), f"{path}: changed non-short-circuit task page-table release loop"
    private_rotated = "__ZN31IGHardwarePerProcessPageTable6415mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory"
    assert struct.unpack_from("<Q", image, value("__ZTV31IGHardwarePerProcessPageTable64") + 16 + 0x120)[0] == value(private_rotated), f"{path}: changed private rotated PPGTT dispatch"
    for address, encoded in (
            (0x1411d, "8b83200100008b8b24010000"),
            (0x141d2, "ff9020010000"),
            (0x752e9, "498b86440200004889831c010000418b864c020000898324010000b00188831c010000")):
        expected = bytes.fromhex(encoded)
        assert image[address:address + len(expected)] == expected, f"{path}: changed rotated PPGTT geometry/commit edge at {address:#x}"
    shrink_range = "__ZN31IGHardwarePerProcessPageTable6411shrinkRangeERK14IGAddressRange"
    for caller, address in (
            ("__ZN31IGHardwarePerProcessPageTable648mapRangeERK14IGAddressRangeyy", 0xd19e),
            ("__ZN31IGHardwarePerProcessPageTable6415mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory", 0xd7e2),
            ("__ZN31IGHardwarePerProcessPageTable6410unmapRangeERK14IGAddressRange", 0xdb18),
            ("__ZN31IGHardwarePerProcessPageTable6413mapRangeDummyERK14IGAddressRangey", 0xdbec),
            ("__ZN31IGHardwarePerProcessPageTable6421mapDescriptorForRangeERK14IGAddressRangePN10IGPagePool14PageDescriptorE", 0xdd58)):
        assert direct_branches(caller, shrink_range) == [address], f"{path}: changed selected PPGTT pruning entry: {caller}"
    for address, encoded in ((0xd8ae, "4889141966ff4010"),
                             (0xd8dc, "31d241f774240c"),
                             (0xd915, "31db"), (0xd911, "b301eb02")):
        expected = bytes.fromhex(encoded)
        assert image[address:address + len(expected)] == expected, f"{path}: changed archived rotated PPGTT store/divisor/result anchor at {address:#x}"
    shrink_leaf = "__ZN31IGHardwarePerProcessPageTable6411shrinkLevelINS_10LevelEntryILm9E17GTTPageTableEntryEENS1_ILm9E21GTTPageDirectoryEntryEEEEbRT_yPT0_ym"
    assert direct_branches(shrink_leaf, "__ZN10IGPagePool14PageDescriptor7releaseEv") == [0xcf1b], f"{path}: changed pruned-table descriptor release edge"
    assert direct_branches("__ZN10IGPagePool14PageDescriptor7releaseEv", "__ZN10IGPagePool11releasePageEPKNS_14PageDescriptorE") == [0xbb7d], f"{path}: changed final-reference pool retirement edge"
    assert image[0xcf0b:0xcf13] == bytes.fromhex("48 89 3c ce 66 ff 4a 10"), f"{path}: changed parent PTE/count before descriptor release"
    # Selected configuration window, not a complete populateAccelConfig audit.
    config = value("__ZN16IntelAccelerator19populateAccelConfigEP13IOAccelConfig")
    assert config <= 0x275be < 0x2760d <= next_symbol(config), f"{path}: changed ring-size validation owner"
    assert hashlib.sha256(image[0x275be:0x2760d]).hexdigest() == "fe215a9a0ba9b8f6bd9c8ccdf56799c5aff7f7af98f6c33167164e1c22ae71b9", f"{path}: changed ring-size power-of-two validation/fallback window"
    assert image[0x27cc8:0x27cd4] == bytes.fromhex("41 c7 84 24 9c 11 00 00 10 00 00 00"), f"{path}: changed firmware ring-size override"
    assert image[0x41da6:0x41dac] == bytes.fromhex("ff 90 60 01 00 00"), f"{path}: changed qword pending-TLB emission"
    assert direct_branches("__ZN20IGHardwareRingBuffer10writeQWordEy", RING_WRITE_BUFFER) == [0x41e31], f"{path}: changed qword software prefix edge"
    assert image[0x41ede:0x41ee4] == bytes.fromhex("ff 90 60 01 00 00"), f"{path}: changed buffer pending-TLB emission"
    assert direct_branches(RING_WRITE_BUFFER, RING_WRITE_BUFFER) == [0x41f72], f"{path}: changed software-prefix recursive emission"
    assert direct_branches(RING_WRITE_BUFFER, "__ZN20IGHardwareRingBuffer16writeFlushAuxTLBEv") == [0x41f26], f"{path}: changed buffer AUX emission"
    assert image[0x41fd2:0x41fd4] == bytes.fromhex("b0 01"), f"{path}: changed unconditional buffer write success"
    flip_wait = "__ZN21IGAccelDisplayMachine16generateFlipWaitEP18IGAccelFIFOChannel"
    assert direct_branches(flip_wait, "__ZN20IGHardwareRingBuffer10writeDWordEj") == [0x7e178], f"{path}: changed unchecked flip dword emission"
    assert direct_branches(flip_wait, RING_WRITE_BUFFER) == [0x7e1e0], f"{path}: changed unchecked flip buffer emission"
    for owner, calls in (
            ("__ZN18IGAccelFIFOChannel18submitStampCommandEv", [0x4c552]),
            ("__ZN18IGAccelFIFOChannel18submitRingCommandsEPjjj", [0x4c63b]),
            ("__ZN21IGAccelDisplayMachine16generateFlipWaitEP18IGAccelFIFOChannel", [0x7e169, 0x7e1d0])):
        assert direct_branches(owner, RING_WAIT_SPACE) == calls, f"{path}: changed selected reservation caller edges: {owner}"
    for address in (0x4c557, 0x4c640):
        assert image[address:address + 3] == bytes.fromhex("84 c0 74"), f"{path}: changed FIFO reservation-result guard"
    assert struct.unpack_from("<Q", image, value(SCHEDULER4_VTABLE) + 16 + 0x150)[0] == value("__ZN12IGScheduler416checkForProgressE10IGHwCsType"), f"{path}: changed scheduler4 timeout progress query"
    assert image[0x4177c:0x41782] == bytes.fromhex("ff 90 50 01 00 00"), f"{path}: changed pending-TLB reservation virtual"
    assert image[0x41785:0x4178a] == bytes.fromhex("41 c6 46 6d 01"), f"{path}: changed pre-validation TLB readiness store"
    assert image[0x417b1:0x417b6] == bytes.fromhex("41 c6 46 6e 01"), f"{path}: changed pre-validation AUX readiness store"
    for table, prefix in (
            ("__ZTV20IGHardwareRingBuffer", "__ZN20IGHardwareRingBuffer"),
            ("__ZTV27IGHardwareRingBufferCompute", "__ZN27IGHardwareRingBufferCompute"),
            ("__ZTV24IGHardwareRingBufferMain", "__ZN24IGHardwareRingBufferMain")):
        for slot, suffix in ((0x150, "16getFlushTLBSpaceEv"), (0x160, "13writeFlushTLBEv")):
            assert struct.unpack_from("<Q", image, value(table) + 16 + slot)[0] == value(prefix + suffix), f"{path}: changed TLB reservation/emission pairing: {table}"
    assert image[0x41c70:0x41c76] == bytes.fromhex("ff 90 60 01 00 00"), f"{path}: changed pending-TLB virtual emission"
    assert direct_branches("__ZN20IGHardwareRingBuffer10writeDWordEj", "__ZN20IGHardwareRingBuffer16writeFlushAuxTLBEv") == [0x41cb5], f"{path}: changed pending AUX emission"
    for owner, calls in (
            ("__ZN20IGHardwareRingBuffer13writeFlushTLBEv", [0x429c5, 0x429f8]),
            ("__ZN27IGHardwareRingBufferCompute13writeFlushTLBEv", [0x4e814, 0x4e841]),
            ("__ZN24IGHardwareRingBufferMain13writeFlushTLBEv", [0x851b8, 0x851e5])):
        assert direct_branches(owner, RING_WRITE_BUFFER) == calls, f"{path}: changed TLB command-buffer emission edges: {owner}"
    display_table = value("__ZTV18IGAccelDisplayPipe")
    assert direct_branches("__ZN15IGMemoryManager22updatePageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap", "__ZN19IGHardwarePageTable11updateRangeERK14IGAddressRangePK16IGAccelMemoryMap") == [0xf865], f"{path}: changed task update fanout edge"
    for address, expected in ((0xf86a, "41 20 c4"), (0x1483d, "ff 90 28 01 00 00"), (0x14843, "84 c0")):
        encoded = bytes.fromhex(expected)
        assert image[address:address + len(encoded)] == encoded, f"{path}: changed update result/remap anchor at {address:#x}"
    assert image[0x148d6:0x148db] == b"\xe8" + struct.pack("<i", 0x2d1d8 - 0x148db), f"{path}: changed update flush notification edge"
    resource_table = value("__ZTV15IGAccelResource")
    assert struct.unpack_from("<Q", image, resource_table + 16 + 0x178)[0] == value("__ZN15IGAccelResource8completeEv"), f"{path}: changed color-resolve concrete completion target"
    assert resource_table + 16 + 0x198 == 0xd9578, f"{path}: moved inherited resource channel cleanup slot"
    map_table = value("__ZTV16IGAccelMemoryMap")
    wait_barrier = "__ZN16IntelAccelerator20barrierForWaitEventsEbP18IGAccelFIFOChannel"
    assert image[0x2bc25:0x2bc2b] == bytes.fromhex("ff 90 f0 01 00 00"), f"{path}: changed event-barrier packet virtual"
    assert image[0x2bc2b:0x2bc2f] == bytes.fromhex("84 c0 74 05"), f"{path}: changed event-barrier emission fallback"
    assert direct_branches(wait_barrier, "__ZN19IGAccelEventMachine11finishEventEP12IOAccelEventj") == [0x2bc47], f"{path}: changed wait-barrier aggregate-event fallback"
    ccs_submit = "__ZN16IntelAccelerator16submitCCSResolveEP15IGAccelResourceP22color_resolve_params_tRK8IGVectorI11blit_rect_t25IGIOMallocAllocatorPolicyEP11IGAccelTask"
    ccs_resource = "__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveTypehh"
    depth_resource = "__ZN15IGAccelResource18submitDepthResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask17EIntelResolveTypehhtt"
    depth_submit = "__ZN16IntelAccelerator18submitDepthResolveEP22depth_resolve_params_tP11IGAccelTask"
    depth_client = "__ZN23IGAccelSharedUserClient13depth_resolveEPvy"
    depth_metal = "__Z19depthStencilResolveR16IOAccelResource2RKN14IntelMTLRender20sResolveResourceDescEP11IGAccelTaskR16IntelAcceleratorPN15IGAccelResource17ResourceInfoEntryEbb"
    assert direct_branches(depth_client, depth_submit) == [0x788e6], f"{path}: changed locked user-client depth publication edge"
    assert direct_branches(depth_metal, depth_resource) == [0x4f1c5], f"{path}: changed Metal depth resource dispatch"
    for address, expected in ((0x78578, "48 8b bb 88 00 00 00"), (0x786ab, "48 8b bb 88 00 00 00"), (0x78924, "45 31 ff"), (0x4f1d0, "84 c0 b8 0a 00 00 00 0f 45 c1")):
        encoded = bytes.fromhex(expected)
        assert image[address:address + len(encoded)] == encoded, f"{path}: changed depth caller lock/status anchor at {address:#x}"
    depth_assembler = "__Z14resolve_hiz_g7P25IOAccelCommandBufferPool2P14IGMappedBufferP22depth_resolve_params_tR15resolve_phase_tRjS7_yb"
    assert direct_branches(depth_submit, "__ZN16IntelAccelerator20barrierForWaitEventsEbP18IGAccelFIFOChannel") == [0x2c74e], f"{path}: changed depth aggregate barrier edge"
    assert direct_branches(depth_submit, depth_assembler) == [0x2c7b9], f"{path}: changed depth chunk assembly edge"
    assert image[0x2c833:0x2c83d] == bytes.fromhex("83 7d d4 0e 0f 85 16 ff ff ff"), f"{path}: changed depth software-phase submission loop"
    assert direct_branches(depth_resource, "__ZN16IntelAccelerator18submitDepthResolveEP22depth_resolve_params_tP11IGAccelTask") == [0x744d7], f"{path}: changed depth resolve submission edge"
    assert direct_branch_candidates(image, value(depth_resource), next_symbol(value(depth_resource)), 0x757b2, external_relocation_offsets) == [0x741eb, 0x7420b], f"{path}: changed selected depth event-vector growth copies"
    assert direct_branch_candidates(image, value(depth_resource), next_symbol(value(depth_resource)), 0x7582a, external_relocation_offsets) == [0x7430e, 0x74331], f"{path}: changed selected depth event collection copies"
    color_resolve = "__ZN23IGAccelSharedUserClient13color_resolveEPvy"
    assert direct_branches(color_resolve, "__ZN23IGAccelSharedUserClient12bindResourceEP15IGAccelResource") == [0x78d95], f"{path}: changed color-resolve bind edge"
    assert image[0x799cf:0x799d3] == bytes.fromhex("84 c0 74 1c"), f"{path}: changed bind prepare-result admission"
    assert image[0x799e5:0x799eb] == bytes.fromhex("ff 90 90 01 00 00"), f"{path}: changed admitted resource channel virtual"
    assert direct_branches(color_resolve, ccs_resource) == [0x78f23], f"{path}: changed locked user-client CCS dispatch"
    for address, expected in ((0x78a08, "48 8b bb 88 00 00 00"), (0x78e35, "48 8b bb 88 00 00 00"), (0x78f28, "31 c9 84 c0 41 be c2 02 00 e0 44 0f 45 f1")):
        encoded = bytes.fromhex(expected)
        assert image[address:address + len(encoded)] == encoded, f"{path}: changed color-resolve lock/error anchor at {address:#x}"
    ccs_planes = "__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveType"
    ccs_enable = "__ZN15IGAccelResource36enableRenderCompressionWithAccelTaskEPNS_17ResourceInfoEntryEyR16IntelAcceleratorP11IGAccelTaskhhb"
    assert direct_branches(ccs_planes, ccs_resource) == [0x740d8], f"{path}: changed per-plane CCS dispatch"
    assert direct_branches(ccs_enable, ccs_resource) == [0x739d6], f"{path}: changed compression-enable resolve dispatch"
    for address, expected in ((0x740dd, "41 20 c5"), (0x739ad, "0c 02"), (0x739af, "88 83 91 00 00 00")):
        encoded = bytes.fromhex(expected)
        assert image[address:address + len(encoded)] == encoded, f"{path}: changed CCS aggregation/pre-resolve state at {address:#x}"
    assert direct_branches(ccs_resource, ccs_submit) == [0x73e99], f"{path}: changed resource CCS submission edge"
    for address, expected in ((0x73c1a, "48 85 c0"), (0x73c1d, "74 1d"), (0x73d08, "4d 89 0c 24"), (0x73eb6, "41 c7 46 64 00 00 00 00"), (0x73f0f, "b0 01")):
        encoded = bytes.fromhex(expected)
        assert image[address:address + len(encoded)] == encoded, f"{path}: changed archived unrepaired CCS allocation/result anchor at {address:#x}"
    assert direct_branches(ccs_submit, "__ZN15IGAccelResource22updateMappingCacheTypeEj") == [0x2c94b], f"{path}: changed CCS cache update edge"
    assert direct_branches(ccs_submit, "__Z11resolve_ccsP25IOAccelCommandBufferPool2P14IGMappedBufferP22color_resolve_params_tbRK8IGVectorI11blit_rect_t25IGIOMallocAllocatorPolicyE") == [0x2cad0], f"{path}: changed CCS resolve assembly edge"
    assert direct_branches("__ZN15IGAccelResource22updateMappingCacheTypeEj", "__ZN16IGAccelMemoryMap15updateCacheTypeEj") == [0x751cf], f"{path}: changed resource cache update edge"
    assert direct_branches("__ZN19IGAccelVideoContext30updateResourceMappingCacheTypeEP15IGAccelResource", "__ZN15IGAccelResource22updateMappingCacheTypeEj") == [0x78105], f"{path}: changed video resource cache update edge"
    assert struct.unpack_from("<Q", image, map_table + 16 + 0x180)[0] == value("__ZN16IGAccelMemoryMap18updateGPUPageTableEv"), f"{path}: changed mapping update virtual"
    assert direct_branches("__ZN16IGAccelMemoryMap18updateGPUPageTableEv", "__ZN15IGMemoryManager22updatePageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap") == [0x114f5], f"{path}: changed mapping update manager edge"
    assert image[0x115a8:0x115ae] == bytes.fromhex("89 b7 14 01 00 00"), f"{path}: changed pre-update cache-type store"
    assert image[0x115ba:0x115c0] == bytes.fromhex("ff a0 80 01 00 00"), f"{path}: changed cache-type update tail dispatch"
    private_init = "__ZN29IGHardwarePerProcessPageTable15initWithOptionsEP16IntelAcceleratorP11IGAccelTaskj"
    for method, call in (("__ZN31IGHardwarePerProcessPageTable3215initWithOptionsEP16IntelAcceleratorP11IGAccelTask", 0x11d4e),
                         ("__ZN31IGHardwarePerProcessPageTable6415initWithOptionsEP16IntelAcceleratorP11IGAccelTask", 0xccf5)):
        assert direct_branches(method, private_init) == [call], f"{path}: changed private page-table option initialization edge"
    assert image[0x11d4c:0x11d4e] == bytes.fromhex("31 c9"), f"{path}: changed 32-bit per-entry synchronization option"
    assert image[0xcce6:0xcceb] == bytes.fromhex("b9 01 00 00 00"), f"{path}: changed 64-bit descriptor synchronization option"
    for method, target, call in (
            ("__ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask", "__ZN31IGHardwarePerProcessPageTable3211withOptionsEP16IntelAcceleratorP11IGAccelTask", 0xf8ff),
            ("__ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask", "__ZN31IGHardwarePerProcessPageTable6411withOptionsEP16IntelAcceleratorP11IGAccelTask", 0xf90d),
            ("__ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask", "__ZN29IGHardwarePerProcessPageTable15synchronizeWithI25IGHardwareGlobalPageTableEEvPKT_RK14IGAddressRangeb", 0xf93c),
            ("__ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask", "__ZN29IGHardwarePerProcessPageTable15synchronizeWithIS_EEvPKT_RK14IGAddressRangeb", 0xf969)):
        assert direct_branches(method, target) == [call], f"{path}: changed native per-task page-table factory/synchronization edge"
    # Whole-body hashes above pin these already-reviewed methods. These
    # selected anchors connect the manager policy to task/factory selection;
    # they do not observe a live device's property or prove GPU acceptance.
    for address, encoded in (
            (0x791b, "498b86601200008a8801010000888bd9020000"),
            (0x82cc, "80bfd9020000000f94c08d440001"),
            (0xea25, "c745e040000000"),
            (0xeb88, "837de020410f948601010000"),
            (0xf8ee, "83f803741383f801754b")):
        expected = bytes.fromhex(encoded)
        assert image[address:address + len(expected)] == expected, f"{path}: changed PageTableMode manager/task/factory policy at {address:#x}"
    mode_property = b"PageTableMode\0"
    assert image[0x9198d:0x9198d + len(mode_property)] == mode_property, f"{path}: changed page-table mode property name"
    global_table = value("__ZTV25IGHardwareGlobalPageTable")
    assert image[0xec45:0xec5d] == bytes.fromhex("b8 00 00 00 40 48 89 83 c0 00 00 00 b9 00 00 00 be 48 89 8b c8 00 00 00"), f"{path}: changed manager fixed nonempty constructor range"
    for bits in (32, 64):
        table = value(f"__ZTV31IGHardwarePerProcessPageTable{bits}")
        getter = value(f"__ZN31IGHardwarePerProcessPageTable{bits}31getPageTableRootPhysicalAddressEPy")
        assert struct.unpack_from("<Q", image, table + 16 + 0x148)[0] == getter, f"{path}: changed PPGTT root getter virtual"
    assert struct.unpack_from("<Q", image, value("__ZTV31IGHardwarePerProcessPageTable32") + 16 + 0x130)[0] == value("__ZN31IGHardwarePerProcessPageTable3210unmapRangeERK14IGAddressRange"), f"{path}: changed 32-bit init unmap virtual"
    for call in (0x11d6e, 0x11d8b, 0x11da8):
        assert image[call:call + 6] == bytes.fromhex("ff 90 30 01 00 00"), f"{path}: changed 32-bit init range-unmap edge"
    assert image[0x7c29b:0x7c2b3] == bytes.fromhex("498b4558488bb860020000488b07488d75b0ff9048010000"), f"{path}: changed context task/private-table root snapshot edge"
    expand32 = "__ZN31IGHardwarePerProcessPageTable326expandE19GTTVirtualAddress32"
    assert direct_branches(expand32, "__ZN10IGPagePool14PageDescriptor7releaseEv") == [0x12646], f"{path}: changed 32-bit expansion software-index allocation rollback"
    assert image[0x1264f:0x12656] == bytes.fromhex("48 c7 00 00 00 00 00"), f"{path}: changed 32-bit rollback descriptor clear"
    accelerator_table = value("__ZTV16IntelAccelerator")
    assert struct.unpack_from("<Q", image, value("__ZTV23IGAccelSharedUserClient") + 16 + 0x990)[0] == value("__ZN23IGAccelSharedUserClient11sharedStartEv"), f"{path}: changed concrete Shared-start virtual"
    for slot, name in ((0x998, "__ZN16IntelAccelerator17createUserGPUTaskEv"),
                       (0x9d0, "__ZN16IntelAccelerator19createKernelGPUTaskEv")):
        assert struct.unpack_from("<Q", image, accelerator_table + 16 + slot)[0] == value(name), f"{path}: changed concrete task factory virtual"
    assert direct_branches("__ZN11IGAccelTask11withOptionsEP16IntelAccelerator", "__ZN11IGAccelTask15initWithOptionsEP16IntelAccelerator") == [0x7870], f"{path}: changed task factory initialization edge"
    assert struct.unpack_from("<Q", image, global_table + 16 + 0x140)[0] == value("__ZNK25IGHardwareGlobalPageTable4readEyRyS0_"), f"{path}: changed global PTE read virtual"
    for method, target, call in (
            ("__ZN29IGHardwarePerProcessPageTable15synchronizeWithI25IGHardwareGlobalPageTableEEvPKT_RK14IGAddressRangeb", "__ZN29IGHardwarePerProcessPageTable20synchronizeEachEntryEPK19IGHardwarePageTableRK14IGAddressRangeb", 0x12c99),
            ("__ZN29IGHardwarePerProcessPageTable15synchronizeWithIS_EEvPKT_RK14IGAddressRangeb", "__ZN29IGHardwarePerProcessPageTable20synchronizeEachEntryEPK19IGHardwarePageTableRK14IGAddressRangeb", 0x12d8a),
            ("__ZN29IGHardwarePerProcessPageTable15synchronizeWithIS_EEvPKT_RK14IGAddressRangeb", "__ZN29IGHardwarePerProcessPageTable25synchronizePageDescriptorEPKS_RK14IGAddressRangeb", 0x12da5)):
        assert direct_branches(method, target) == [call], f"{path}: changed page-table synchronization helper edge"
    for slot, method in ((0x118, GLOBAL_MAP_RANGE), (0x120, GLOBAL_MAP_ROTATED), (0x138, GLOBAL_MAP_DUMMY)):
        assert struct.unpack_from("<Q", image, global_table + 16 + slot)[0] == value(method), f"{path}: changed global commit-range mapping virtual"
    assert direct_branches("__ZN15IGMemoryManager26commitIntoPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap", "__ZN19IGHardwarePageTable11commitRangeERK14IGAddressRangePK16IGAccelMemoryMap") == [0xf639], f"{path}: changed manager-to-page-table commit edge"
    assert image[0x143ce:0x143d2] == bytes.fromhex("84 c0 74 4b"), f"{path}: changed segment mapping failure branch"
    assert image[0x14487:0x14491] == bytes.fromhex("ff 90 38 01 00 00 49 8b 7f 10"), f"{path}: changed ignored suffix-dummy result edge"
    assert struct.unpack_from("<Q", image, map_table + 16 + 0x170)[0] == value("__ZN16IGAccelMemoryMap22commitIntoGPUPageTableEv"), f"{path}: changed mapping commit virtual"
    assert direct_branches("__ZN16IGAccelMemoryMap22commitIntoGPUPageTableEv", "__ZN15IGMemoryManager26commitIntoPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap") == [0x11275], f"{path}: changed mapping-to-manager commit edge"
    assert struct.unpack_from("<Q", image, map_table + 16 + 0x178)[0] == value("__ZN16IGAccelMemoryMap23releaseFromGPUPageTableEv"), f"{path}: changed mapping release virtual"
    assert struct.unpack_from("<Q", image, map_table + 16 + 0x160)[0] == value("__ZN16IGAccelMemoryMap21freeGPUVirtualAddressEv"), f"{path}: changed Intel mapping VA-free override"
    assert image[0x11116:0x1111c] == bytes.fromhex("ff 90 70 01 00 00"), f"{path}: changed Intel VA-free base delegation"
    assert direct_branches("__ZN16IGAccelMemoryMap23releaseFromGPUPageTableEv", "__ZN15IGMemoryManager27releaseFromPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap") == [0x113b5], f"{path}: changed mapping-to-manager release edge"
    sys_memory_table = value("__ZTV16IGAccelSysMemory")
    assert value(ACCELERATOR_VTABLE) + 16 + 0x940 == 0xd19b0, f"{path}: changed Intel video recovery import slot"
    for slot, method in ((0x9d8, "__ZN16IntelAccelerator15systemWillSleepEv"),
                         (0x9e0, "__ZN16IntelAccelerator13systemDidWakeEv")):
        assert struct.unpack_from("<Q", image, value(ACCELERATOR_VTABLE) + 16 + slot)[0] == value(method), f"{path}: changed Intel power override identity"
    assert value(ACCELERATOR_VTABLE) + 16 + 0x968 == 0xd19d8, f"{path}: changed Intel system recovery import slot"
    assert struct.unpack_from("<Q", image, sys_memory_table + 16 + 0x1b0)[0] == value("__ZN16IGAccelSysMemory4wireEv"), f"{path}: changed Intel sys-memory wire override"
    assert image[0x128a4:0x128ab] == bytes.fromhex("48 8b 05 95 58 0b 00"), f"{path}: changed wire base-table import load"
    assert image[0x128ab:0x128b1] == bytes.fromhex("ff 90 c0 01 00 00"), f"{path}: changed explicit sys-memory base wire delegation"
    assert image[0x129b4:0x129ba] == bytes.fromhex("ff 90 c8 01 00 00"), f"{path}: changed explicit sys-memory base unwire delegation"
    assert struct.unpack_from("<Q", image, sys_memory_table + 16 + 0x1b8)[0] == value("__ZN16IGAccelSysMemory6unwireEv"), f"{path}: changed Intel sys-memory unwire override"
    assert struct.unpack_from("<Q", image, map_table + 0xa0)[0] == value("__ZN16IGAccelMemoryMap4freeEv"), f"{path}: changed Intel memory-map free override"
    assert image[0x10e68:0x10e6e] == bytes.fromhex("ff a0 a0 00 00 00"), f"{path}: changed memory-map base free delegation"
    assert struct.unpack_from("<Q", image, resource_table + 16 + 0x178)[0] == value("__ZN15IGAccelResource8completeEv"), f"{path}: changed Intel resource complete override"
    assert image[0x753e6:0x753ec] == bytes.fromhex("ff a0 88 01 00 00"), f"{path}: changed resource base complete header-slot delegation"
    event_table = value("__ZTV19IGAccelEventMachine")
    for slot, method in ((0x138, "__ZN19IGAccelEventMachine14getStampOffsetEi"),
                         (0x2a0, "__ZN19IGAccelEventMachine10writeStampEiP17vendevtCommandRecj")):
        assert struct.unpack_from("<Q", image, event_table + 16 + slot)[0] == value(method), f"{path}: changed Intel stamp record virtual target"
    assert image[0x15f16:0x15f1c] == bytes.fromhex("89 03 44 89 73 04"), f"{path}: changed CPU stamp record stores"
    for slot, method in ((0x8b8, "__ZN18IGAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2"),
                         (0x8d8, "__ZN18IGAccelDisplayPipe16beginTransactionEP12IOAccelEvent")):
        assert struct.unpack_from("<Q", image, display_table + 16 + slot)[0] == value(method), f"{path}: changed Intel display transaction override"
    assert image[0x80ecd:0x80ed4] == bytes.fromhex("c6 83 3a 13 00 00 01"), f"{path}: changed Intel transaction state store"
    assert image[0x80ee1:0x80ee7] == bytes.fromhex("ff 90 c8 08 00 00"), f"{path}: changed explicit legacy-table submit delegation"
    # Explicit semantic anchors: the periodic callback uses collection count,
    # whereas enable/disable maintain a separate unchecked reference counter.
    # Do not replace one with the other or infer teardown serialization merely
    # because all three producer methods acquire scheduler+0x440.
    periodic_anchors = (
        ("__ZN11IGScheduler33enablePeriodicEventTimerInterruptEP22IOInterruptEventSource", (
            (0xd, "48 8b bf 40 04 00 00"),
            (0x2c, "48 8b 83 50 04 00 00"),
            (0x37, "48 89 8b 50 04 00 00"),
            (0x62, "ff 90 d8 01 00 00"))),
        ("__ZN11IGScheduler34disablePeriodicEventTimerInterruptEP22IOInterruptEventSource", (
            (0xd, "48 8b bf 40 04 00 00"),
            (0x33, "48 8d 48 ff"),
            (0x37, "48 89 8b 50 04 00 00"),
            (0x4e, "ff 90 58 01 00 00"))),
        ("__ZN11IGScheduler28handlePeriodicTimerInterruptEP18IOTimerEventSource", (
            (0xd, "48 8b bf 40 04 00 00"),
            (0x4a, "ff 93 e0 01 00 00"),
            (0x65, "ff 90 f0 01 00 00"),
            (0x7f, "ff 90 d8 01 00 00"))),
    )
    for name, anchors in periodic_anchors:
        start = value(name)
        for offset, encoded in anchors:
            expected = bytes.fromhex(encoded)
            assert image[start + offset:start + offset + len(expected)] == expected, \
                f"{path}: changed periodic counter/lock/rearm semantic anchor: {name}+{offset:#x}"
    # Fallback source is created with the event machine as raw callback owner.
    # Native add/remove return statuses are ignored. Source retention by an
    # OSSet must not be equated with retention of that callback owner.
    for address, encoded in (
            (0x15cbc, "48 8d 35 23 00 00 00"),
            (0x15cc3, "48 89 df"),
            (0x15ccf, "48 89 83 30 0d 00 00"),
            (0x15d90, "ff 91 40 01 00 00"),
            (0x15d96, "c6 83 88 0d 00 00 01"),
            (0x15d2a, "ff 91 48 01 00 00"),
            (0x15d30, "48 8b bb 30 0d 00 00"),
            (0x15d42, "48 c7 83 30 0d 00 00 00 00 00 00")):
        expected = bytes.fromhex(encoded)
        assert image[address:address + len(expected)] == expected, \
            f"{path}: changed fallback source constructor/unchecked attachment/release"
    assert 0x15cbc + 7 + struct.unpack_from("<i", image, 0x15cbc + 3)[0] == \
        value("__ZN19IGAccelEventMachine29handleSchedulerStampInterruptEP22IOInterruptEventSourcei"), \
        f"{path}: changed fallback source callback constructor target"
    # Symbol boundaries alone merge unnamed functions into init/free. Pin the
    # disassembled function windows separately, not as one fictitious body.
    scheduler_init = value("__ZN11IGScheduler15initWithOptionsEjyP22IOGraphicsAccelerator2")
    scheduler_free = value("__ZN11IGScheduler4freeEv")
    for start, length, digest in (
            (scheduler_init, 0x162, "b5835cb1b8a29f7502434b8a5e0660adc9f0d96f5e261c4455dd6c4f5570b46e"),
            (scheduler_init + 0x162, 0xe0, "42074790409f72ce5a8c25e16d719c3f976758d72d34798e4cd152d0b9c89688"),
            (scheduler_free, 0x24, "a967c406cd60ae66a315e23af8c8490c8e1986b6822a5446cee93853a850ff72")):
        assert hashlib.sha256(image[start:start + length]).hexdigest() == digest, \
            f"{path}: changed scheduler timer construction/cleanup window"
    cleanup = scheduler_init + 0x162
    for name, length, digest in (
            ("__ZN12IGScheduler44freeEv", 0xa2, "61b83374f994845f22740f343019b57d0330405fd7e57fe66e5608f700a1a388"),
            ("__ZN12IGScheduler54freeEv", 0x126, "39a1fffea7ae56ea13daabfffe6722828bb990b16eeb8be4cb707c2e55c0b44a")):
        start = value(name)
        assert next_symbol(start) - start == length and hashlib.sha256(image[start:start + length]).hexdigest() == digest, \
            f"{path}: changed reviewed derived scheduler free body"
        lea = start + (0x88 if length == 0xa2 else 0x10b)
        assert image[lea:lea + 3] == bytes.fromhex("48 8d 05"), f"{path}: changed base scheduler vtable reference"
        table = lea + 7 + struct.unpack_from("<i", image, lea + 3)[0]
        assert struct.unpack_from("<Q", image, table + 0xa0)[0] == scheduler_free, \
            f"{path}: derived scheduler free no longer delegates to base free"
    scheduler5_free = value("__ZN12IGScheduler54freeEv")
    assert image[scheduler5_free + 0x100:scheduler5_free + 0x10b] == bytes.fromhex("49 c7 86 80 0a 00 00 00 00 00 00"), \
        f"{path}: changed private workloop clear before inherited cleanup"
    scheduler5_init = value("__ZN12IGScheduler519initWithAcceleratorEP22IOGraphicsAccelerator2")
    scheduler4_init = value(SCHEDULER4_INIT)
    find = bytes.fromhex("49 8b 04 24 4c 89 e7 ff 90 90 00 00 00 45 31 f6")
    assert image[scheduler4_init:scheduler4_init + 0xaf].count(find) == 1 and \
        image[scheduler4_init + 0x93:scheduler4_init + 0xa3] == find, \
        f"{path}: scheduler premature-free patch must have one exact bounded match"
    # The nearest symbol span additionally includes an unnamed GuC thunk.
    assert hashlib.sha256(image[scheduler4_init:scheduler4_init + 0xaf]).hexdigest() == \
        "77b13fd9de6707590ce67777bfe7dfdc4f475072740e0ac1baf1746ad3eda471", \
        f"{path}: changed reviewed scheduler-4 initialization window"
    assert image[scheduler4_init + 0x9a:scheduler4_init + 0xa0] == bytes.fromhex("ff 90 90 00 00 00"), \
        f"{path}: changed scheduler-4 partial-init free dispatch"
    assert struct.unpack_from("<Q", image, value(SCHEDULER4_VTABLE) + 16 + 0x90)[0] == value("__ZN12IGScheduler44freeEv"), \
        f"{path}: changed scheduler-4 partial-init effective free target"
    create = value("__ZN11IGScheduler6createEP16IntelAccelerator")
    deleting = value("__ZN12IGScheduler4D0Ev")
    assert next_symbol(deleting) - deleting == 0x22 and hashlib.sha256(image[deleting:deleting + 0x22]).hexdigest() == \
        "f177eac0a487ced353d1984925cd44048aabaad94d8075762b40dc7e8782d13d", \
        f"{path}: changed complete scheduler-4 deleting destructor"
    assert struct.unpack_from("<Q", image, value(SCHEDULER4_VTABLE) + 16 + 8)[0] == deleting, \
        f"{path}: changed effective scheduler-4 deleting destructor"
    free_links = []
    for index in range(dysymtab[17]):
        relocation, bits = struct.unpack_from("<iI", image, dysymtab[16] + index * 8)
        if relocation in (0xc81e0, deleting + 0x1d):
            free_links.append((relocation, names[bits & 0xffffff], bits >> 24))
    assert sorted(free_links) == sorted(((0xc81e0, "__ZTV8OSObject", 0x0e),
                                       (deleting + 0x1d, "__ZN8OSObjectdlEPvm", 0x2d))), \
        f"{path}: changed scheduler base-free/deallocation imported links"
    for name, length, digest in (
            ("__ZN5IGGuC20sendHostToGucMessageEPK18IGHostToGucMessagejU13block_pointerFvvE", 0x122,
             "0de3a1744332cb6811d2d75a0e5d5e3998e30746f4c56d67720f2d5860d1138a"),
            ("__ZN5IGGuC12ringDoorbellE10IGHwCsType", 0x12a,
             "3f0d630e69161b4fd8c80def32d4a3dcbee2e9eb9f894a40bae00c94f179bfa1")):
        start = value(name)
        assert next_symbol(start) - start == length and hashlib.sha256(image[start:start + length]).hexdigest() == digest, \
            f"{path}: changed complete IGGuC DPSM producer body"
    for call in (0x1a1b1, 0x1c3f7):
        assert image[call] == 0xe8 and call + 5 + struct.unpack_from("<i", image, call + 1)[0] == value("__ZN16IntelAccelerator13dpsmKickTimerEv"), \
            f"{path}: changed IGGuC producer to DPSM kick edge"
    for address, instruction in ((0x1a21a, "48 8b 8f 40 12 00 00"),
                                  (0x1a221, "89 81 80 c1 00 00"),
                                  (0x1a227, "c7 81 f0 01 19 00 01 00 00 00")):
        assert image[address:address + len(bytes.fromhex(instruction))] == bytes.fromhex(instruction), \
            f"{path}: changed physical IGGuC H2G MMIO branch inventory"
    kick_irq = value("__ZN12IGScheduler523handleKickDPSMInterruptEP22IOInterruptEventSourcei")
    assert next_symbol(kick_irq) - kick_irq == 0xe and hashlib.sha256(image[kick_irq:kick_irq + 0xe]).hexdigest() == \
        "501e06bc18a51b099b953b2adc98887bb848e2ed64fccc93e979f1ac27bef11c", \
        f"{path}: changed complete scheduler-5 DPSM kick callback body"
    branch = kick_irq + 9
    assert image[branch] == 0xe9 and branch + 5 + struct.unpack_from("<i", image, branch + 1)[0] == value("__ZN16IntelAccelerator13dpsmKickTimerEv"), \
        f"{path}: changed scheduler-5 callback to DPSM producer edge"
    for name, length, digest in (
            (DPSM_IDLE_TIMER, 0x96, "2be716f37bbbd3ba2df81c63a9cc0fe5d843f6ffc0fde9649fac540d542e477c"),
            ("__ZN16IntelAccelerator13dpsmKickTimerEv", 0x7e, "e22ef5bc66aaaa0ab4cf4eb802406d1d843bbcd8c33981720b4e1fc926ab9354"),
            ("__ZN16IntelAccelerator10dpsmIsIdleEv", 0xe, "9f3eac3e1a7c8eb1c2d2159cf3d8805748718c86ba9856da126e328d93d4fe93")):
        start = value(name)
        assert next_symbol(start) - start == length and hashlib.sha256(image[start:start + length]).hexdigest() == digest, \
            f"{path}: changed complete DPSM timer/state consumer body"
    assert hashlib.sha256(image[0x2463f:0x246a9]).hexdigest() == \
        "972f26b62a647876bbe8e0da6240f34246bfc55507c9b9195be762ef65c35a28", \
        f"{path}: changed reviewed post-scheduler DPSM construction subsection"
    for start, length, digest in (
            (value("__ZN16IntelAccelerator4freeEv"), 0x24,
             "0eb67d3ff65227b0608257b4664d88c8d816ed1e66adba8e6fdc7b477bb4a2e6"),
            (0x23dfe, 0xce, "80ea784abb76d4891a7aa3f1d03287bb7beb5c62787c76335baf808a4018623b"),
            (value(STOP), 0xc4, "1a42a35a3faa031f11f24ed7992fd6378f38d3f36da1c620c745eac39466f619")):
        assert hashlib.sha256(image[start:start + length]).hexdigest() == digest, \
            f"{path}: changed reviewed accelerator free/helper/engine-stop window"
    for table, target in ((SCHEDULER4_VTABLE, "__ZN12IGScheduler414waitForGpuIdleEv"),
                          (SCHEDULER5_VTABLE, "__ZN12IGScheduler514waitForGpuIdleEv")):
        assert struct.unpack_from("<Q", image, value(table) + 16 + 0x170)[0] == value(target), \
            f"{path}: changed engine-stop scheduler wait virtual"
    assert image[0x2680a:0x26810] == bytes.fromhex("ff 90 18 02 00 00") and \
        image[0x26810:0x2681a] == bytes.fromhex("c7 83 58 14 00 00 01 00 00 00"), \
        f"{path}: changed native engine-stop timer cancellation/stop-state store"
    stop_start = value(ACCELERATOR_STOP)
    assert next_symbol(stop_start) - stop_start == 0x3e4 and \
        hashlib.sha256(image[stop_start:stop_start + 0x3e4]).hexdigest() == \
        "ba1ef863b3a8aeb1137a044992ca41770f2145fd55ac3df006440cb2197302ca", \
        f"{path}: changed complete reviewed native accelerator stop body"
    for address, instruction in (
            (0x2650c, "49 8b bf 50 12 00 00 48 8b 07"),
            (0x26529, "ff 91 48 01 00 00"),
            (0x26591, "3d 00 00 80 02"),
            (0x266d8, "4d 85 f6 74 13"),
            (0x266ea, "ff 90 d8 05 00 00")):
        assert image[address:address + len(bytes.fromhex(instruction))] == bytes.fromhex(instruction), \
            f"{path}: changed partial-state/type-5-release/provider-gated inherited-stop branch"
    assert hashlib.sha256(image[0x2473d:0x247f0]).hexdigest() == \
        "5288265edb6d924d92e219315b3d87d4a849a1f5751f16aca0992b5c5df015ad", \
        f"{path}: changed reviewed native start failure epilogue window"
    assert image[0x2477e:0x24786] == bytes.fromhex("31 f6 ff 90 c8 05 00 00"), \
        f"{path}: changed null-provider virtual stop on start failure"
    assert struct.unpack_from("<Q", image, value(ACCELERATOR_VTABLE) + 16 + 0x5c8)[0] == value(ACCELERATOR_STOP), \
        f"{path}: changed effective start-failure stop virtual"
    for call, store in ((0x243f3, "49 89 85 50 12 00 00"),
                        (0x2448e, "49 89 85 50 12 00 00")):
        assert image[call] == 0xe8 and call + 5 + struct.unpack_from("<i", image, call + 1)[0] == create, \
            f"{path}: changed native scheduler creation call"
        assert image[call + 5:call + 12] == bytes.fromhex(store) and image[call + 12:call + 17] == bytes.fromhex("48 85 c0 0f 84"), \
            f"{path}: changed scheduler result store/null branch"
        assert call + 21 + struct.unpack_from("<i", image, call + 17)[0] == 0x2473d, \
            f"{path}: changed scheduler creation null-failure target"
    property_helper = value("__Z15utilGetPropertyIjET_P15IORegistryEntryPKcS0_")
    assert next_symbol(property_helper) - property_helper == 0x18c and \
        hashlib.sha256(image[property_helper:property_helper + 0x18c]).hexdigest() == \
        "9da1339c8d7b6f93f71bf4020792fc4090572cca3bb9046120eb4dc617ef28af", \
        f"{path}: changed reviewed numeric property helper including options override"
    assert image[0x919be:0x919d4] == b"IODeviceTree:/options\0", \
        f"{path}: changed late numeric property override path"
    assert image[0x28216:0x28219] == bytes.fromhex("89 45 dc"), \
        f"{path}: changed late options parsed-value overwrite"
    # Reviewed start subsection, not a claim of full accelerator-start review.
    assert hashlib.sha256(image[0x27a68:0x27b19]).hexdigest() == \
        "aa10449fd9fb97c083b7e98915901350999d241a9af78a7ef23a27bff90a7c5e", \
        f"{path}: changed scheduler-property/firmware-disable override window"
    assert image[0x9505e:0x95072] == b"-disablegfxfirmware\0", \
        f"{path}: changed native scheduler-5 override boot argument"
    # Nearest named-symbol span also includes two unnamed helpers. Only the
    # 0x36 dispatcher window was reviewed as create itself.
    assert hashlib.sha256(image[create:create + 0x36]).hexdigest() == \
        "efc58a38170d12a0f3b2b5c993ab39016364740a93f8816f90c581626696a2b0", \
        f"{path}: changed reviewed scheduler type dispatcher window"
    for offset, target in ((0x20, "__ZN5IGGuC15withAcceleratorEP22IOGraphicsAccelerator2"),
                           (0x26, "__ZN12IGScheduler415withAcceleratorEP22IOGraphicsAccelerator2"),
                           (0x2c, "__ZN12IGScheduler515withAcceleratorEP22IOGraphicsAccelerator2")):
        branch = create + offset
        assert image[branch] == 0xe9 and branch + 5 + struct.unpack_from("<i", image, branch + 1)[0] == value(target), \
            f"{path}: changed scheduler type factory delegation"
    for name, length, digest in (
            ("__ZN5IGGuC15withAcceleratorEP22IOGraphicsAccelerator2", 0x48,
             "5e1ec8f283a501196a7a6649b7f14c4a7aebe71bdb54e4332962ec3c8f401575"),
            ("__ZN12IGScheduler415withAcceleratorEP22IOGraphicsAccelerator2", 0x48,
             "7e58f7befea67c06db244e8699348cca2ab834835393b9f9bf5cfd1b333bae3b"),
            ("__ZN12IGScheduler515withAcceleratorEP22IOGraphicsAccelerator2", 0x48,
             "9e52a23c81a439f5c46163fb8603f9b6d7a19120d4e4b69d3840e5128d58e2e6")):
        start = value(name)
        assert next_symbol(start) - start == length and hashlib.sha256(image[start:start + length]).hexdigest() == digest, \
            f"{path}: changed reviewed scheduler factory allocation/init/release body"
    legacy_factory = value("__ZN5IGGuC15withAcceleratorEP22IOGraphicsAccelerator2")
    legacy_init = value("__ZN5IGGuC15initWithOptionsEP22IOGraphicsAccelerator2")
    for call, target in ((legacy_factory + 0x2c, legacy_init), (legacy_init + 0x24, scheduler_init)):
        assert image[call] == 0xe8 and call + 5 + struct.unpack_from("<i", image, call + 1)[0] == target, \
            f"{path}: changed legacy scheduler factory/base-init provenance edge"
    assert next_symbol(scheduler5_init) - scheduler5_init == 0x17c and \
        hashlib.sha256(image[scheduler5_init:scheduler5_init + 0x17c]).hexdigest() == \
        "d9be84cb035ee2f753d469bdf54151714273201a81b1d30f1ddd6725dd01bda1", \
        f"{path}: changed reviewed scheduler-5 initialization body"
    call = scheduler5_init + 0x26
    assert image[call] == 0xe8 and call + 5 + struct.unpack_from("<i", image, call + 1)[0] == scheduler_init, \
        f"{path}: changed scheduler-5 inherited initialization edge"
    assert image[scheduler5_init + 0x38:scheduler5_init + 0x40] == bytes.fromhex("49 89 84 24 80 0a 00 00"), \
        f"{path}: changed private workloop store after inherited timer setup"
    assert image[scheduler5_init + 0x167:scheduler5_init + 0x16d] == bytes.fromhex("ff 90 90 00 00 00"), \
        f"{path}: changed scheduler-5 failed-init free dispatch"
    assert struct.unpack_from("<Q", image, value(SCHEDULER5_VTABLE) + 16 + 0x90)[0] == scheduler5_free, \
        f"{path}: changed scheduler-5 failed-init effective free target"
    # This is an inventory of the native failure-sensitive ordering, not a
    # claim that cancellation or unchecked removal drains every callback.
    for offset, instruction in (
            (0x19, "ff 90 18 02 00 00"),  # cancel timer
            (0x49, "ff 91 48 01 00 00"),  # remove source
            (0x4f, "49 8b be 48 04 00 00"),  # immediately load timer for release: no status check
            (0x5e, "ff 50 28"),
            (0x61, "49 c7 86 48 04 00 00 00 00 00 00"),
            (0x7e, "49 c7 86 38 04 00 00 00 00 00 00"),
            (0x95, "e8")):
        assert image[cleanup + offset:cleanup + offset + len(bytes.fromhex(instruction))] == bytes.fromhex(instruction), \
            f"{path}: changed scheduler unchecked removal/release ordering"
    for call in (scheduler_init + 0x14c, scheduler_free + 9):
        assert image[call] == 0xe8 and call + 5 + struct.unpack_from("<i", image, call + 1)[0] == cleanup, \
            f"{path}: changed shared scheduler cleanup edge"
    for table, slot, method in (
            (SCHEDULER4_VTABLE, 0x218, "__ZNK11IGScheduler11getWorkLoopEv"),
            (SCHEDULER5_VTABLE, 0x218, "__ZNK12IGScheduler511getWorkLoopEv"),
            (ACCELERATOR_VTABLE, 0x688, "__ZNK22IOGraphicsAccelerator211getWorkLoopEv"),
            (EVENT_MACHINE_VTABLE, 0x240, "__ZN19IGAccelEventMachine20enableStampInterruptEi"),
            (EVENT_MACHINE_VTABLE, 0x248, "__ZN19IGAccelEventMachine21disableStampInterruptEi"),
            (SCHEDULER4_VTABLE, 0x1b0, "__ZN12IGScheduler420enableStampInterruptEi"),
            (SCHEDULER4_VTABLE, 0x1b8, "__ZN12IGScheduler421disableStampInterruptEi")):
        assert struct.unpack_from("<Q", image, value(table) + 16 + slot)[0] == value(method), \
            f"{path}: changed concrete stamp IRQ virtual"
    for owner, target in (
            ("__ZN19IGAccelEventMachine29handleSchedulerStampInterruptEP22IOInterruptEventSourcei", "__ZN16IntelAccelerator17signalStampUpdateERK8IGBitSetILm64EE"),
            ("__ZN16IntelAccelerator17signalStampUpdateERK8IGBitSetILm64EE", TASK_STAMPS),
            ("__ZN19IGAccelEventMachine20enableStampInterruptEi", "__ZN19IGAccelEventMachine20enableStampInterruptEii"),
            ("__ZN19IGAccelEventMachine21disableStampInterruptEi", "__ZN19IGAccelEventMachine21disableStampInterruptEii"),
            ("__ZN12IGScheduler420enableStampInterruptEi", "__ZN26IGHardwareCommandStreamer420enableStampInterruptEi"),
            ("__ZN12IGScheduler421disableStampInterruptEi", "__ZN26IGHardwareCommandStreamer421disableStampInterruptEi"),
            ("__ZN26IGHardwareCommandStreamer420enableStampInterruptEi", "__ZN17IGInterruptBridge15enableInterruptEj"),
            ("__ZN26IGHardwareCommandStreamer421disableStampInterruptEi", "__ZN17IGInterruptBridge16disableInterruptEj"),
            ("__ZN17IGInterruptBridge15enableInterruptEj", "__ZN17IGInterruptBridge15enableInterruptEPNS_15InterruptTraitsE"),
            ("__ZN17IGInterruptBridge16disableInterruptEj", "__ZN17IGInterruptBridge16disableInterruptEPNS_15InterruptTraitsE")):
        assert len(direct_branches(owner, target)) == 1, f"{path}: changed stamp IRQ graph edge"

    accelerator_start = value(ACCELERATOR_START)
    # Reviewed direct callers, not a complete indirect-call reachability proof.
    # Event-machine fallback remains relevant to VF type 4; streamer5 callers
    # must not be mistaken for the admitted scheduler4 implementation.
    periodic_enable = "__ZN11IGScheduler33enablePeriodicEventTimerInterruptEP22IOInterruptEventSource"
    periodic_disable = "__ZN11IGScheduler34disablePeriodicEventTimerInterruptEP22IOInterruptEventSource"
    assert direct_branches(periodic_enable, periodic_disable) == [], \
        f"{path}: imported periodic unlock placeholder became a fictitious local disable edge"
    for owner, target, address in (
            ("__ZN19IGAccelEventMachine20enableStampInterruptEii", periodic_enable, 0x16232),
            ("__ZN19IGAccelEventMachine21disableStampInterruptEii", periodic_disable, 0x162de),
            ("__ZN26IGHardwareCommandStreamer54initEP22IOGraphicsAccelerator2P10IOWorkLoopP12IGScheduler510IGHwCsType", periodic_enable, 0x39c76),
            ("__ZN26IGHardwareCommandStreamer521registerForInterruptsEv", periodic_disable, 0x3a033),
            ("__ZN26IGHardwareCommandStreamer528enableContextSwitchInterruptEv", periodic_disable, 0x3aab4)):
        assert direct_branches(owner, target) == [address], \
            f"{path}: changed reviewed periodic producer call edge"
    for address, encoded in (
            (0x1621e, "48 8b b8 50 12 00 00"),
            (0x16225, "48 8b b3 30 0d 00 00"),
            (0x162cc, "48 8b b8 50 12 00 00"),
            (0x162d3, "48 8b b3 30 0d 00 00")):
        expected = bytes.fromhex(encoded)
        assert image[address:address + len(expected)] == expected, \
            f"{path}: changed fallback scheduler/event-source ownership edge"
    accelerator_start_end = next_symbol(accelerator_start)
    dpsm_failures = []
    cursor = 0
    while True:
        cursor = image.find(DPSM_START_FAILURE_ANCHOR, cursor)
        if cursor < 0:
            break
        dpsm_failures.append(cursor)
        cursor += 1
    if len(dpsm_failures) != 1 or not (
            accelerator_start < dpsm_failures[0] < accelerator_start_end):
        raise AssertionError(
            f"{path}: native post-engine 0x215 failure edge changed")

    # initFirmware reaches the Gen11 scheduler implementation through virtual
    # slot 0x220.  Keep the complete retained native bootstrap chain explicit:
    # every hardware-facing GuC descendant below must remain intercepted by the
    # VF routes checked in source_contract().
    scheduler4_vtable = value(SCHEDULER4_VTABLE)
    load_firmware_slot = struct.unpack_from(
        "<Q", image, scheduler4_vtable + 16 + 0x220)[0]
    if load_firmware_slot != value(SCHEDULER4_LOAD_FIRMWARE):
        raise AssertionError(
            f"{path}: scheduler-4 firmware virtual slot changed")

    # Native start installs a DPSM software timer after engine admission. Its
    # only scheduler decision must dispatch through the routed isGpuIdle slot;
    # the local callback-table endpoint is an exact no-op, not hardware PM.
    for vtable, idle in ((SCHEDULER4_VTABLE, SCHEDULER4_IS_GPU_IDLE),
                         (SCHEDULER5_VTABLE, SCHEDULER5_IS_GPU_IDLE)):
        idle_slot = struct.unpack_from(
            "<Q", image, value(vtable) + 16 + 0x160)[0]
        if idle_slot != value(idle):
            raise AssertionError(f"{path}: {vtable} idle virtual slot changed")
    scheduler4_vtable = value(SCHEDULER4_VTABLE)
    for slot, target in ((0x118, SCHEDULER4_SYSTEM_SLEEP),
                         (0x120, SCHEDULER4_SYSTEM_WAKE)):
        if struct.unpack_from(
                "<Q", image, scheduler4_vtable + 16 + slot)[0] != value(target):
            raise AssertionError(
                f"{path}: scheduler-4 power-state slot {slot:#x} changed")

    # Tahoe's stamp-timeout state machine keeps useful software recovery, but
    # its non-virtual helpers assume ownership of physical engine registers.
    # Prove every Scheduler4 virtual used by eventTimeout first: progress is a
    # constant true result, pause/resume reach exact GuC no-ops, active-context
    # discovery clears every output, and reset preparation is also a no-op.
    # Consequently the classified VF normally reaches encodeDebugInfo directly;
    # the halt/resume routes remain a defensive boundary if that topology ever
    # changes without the pinned vtable and bytecode checks failing first.
    for slot, target in (
            (0x150, SCHEDULER4_CHECK_PROGRESS),
            (0x178, SCHEDULER4_PAUSE),
            (0x180, SCHEDULER4_RESUME),
            (0x188, SCHEDULER4_PREPARE_RESET),
            (0x198, SCHEDULER4_GET_ACTIVE)):
        if struct.unpack_from(
                "<Q", image, scheduler4_vtable + 16 + slot)[0] != value(target):
            raise AssertionError(
                f"{path}: timeout scheduler-4 slot {slot:#x} changed")
    check_progress = image[value(SCHEDULER4_CHECK_PROGRESS):
                           next_symbol(value(SCHEDULER4_CHECK_PROGRESS))]
    if check_progress != bytes.fromhex("55 48 89 e5 b0 01 5d c3"):
        raise AssertionError(f"{path}: scheduler-4 progress result changed")
    if len(direct_branches(SCHEDULER4_PAUSE, GUC_PAUSE)) != 1 or \
            len(direct_branches(SCHEDULER4_RESUME, GUC_RESUME)) != 1:
        raise AssertionError(f"{path}: scheduler-4 pause/resume graph changed")
    for target in (GUC_PAUSE, GUC_RESUME, SCHEDULER4_PREPARE_RESET):
        start = value(target)
        if image[start:next_symbol(start)] != VOID_NOOP_BODY:
            raise AssertionError(f"{path}: native timeout no-op changed: {target}")
    active_start = value(SCHEDULER4_GET_ACTIVE)
    active_body = image[active_start:next_symbol(active_start)]
    if active_body != bytes.fromhex(
            "55 48 89 e5 31 c0 48 89 02 48 89 01 49 89 00 5d c3 90"):
        raise AssertionError(
            f"{path}: scheduler-4 active-context zero result changed")

    timeout_start = value(EVENT_TIMEOUT)
    if struct.unpack_from("<Q", image,
            value(SCHEDULER4_VTABLE) + 16 + 0x148)[0] != value(SCHEDULER4_PUSH):
        raise AssertionError(f"{path}: ring submission scheduler virtual changed")
    push_start = value(SCHEDULER4_PUSH)
    push_body = image[push_start:next_symbol(push_start)]
    if len(direct_branches(SCHEDULER4_PUSH, GUC_SUBMIT_WORK_ITEM)) != 1:
        raise AssertionError(f"{path}: scheduler/GuC submission graph changed")
    if len(direct_branches(CONTEXT_INIT, FIFO_FACTORY)) != 1 or \
            len(direct_branches(FIFO_FACTORY, FIFO_INIT)) != 1:
        raise AssertionError(f"{path}: FIFO/ring producer ownership graph changed")
    fifo_init_start = value(FIFO_INIT)
    fifo_init_body = image[fifo_init_start:next_symbol(fifo_init_start)]
    if fifo_init_body.count(bytes.fromhex(
            "48 89 97 30 01 00 00 48 8b 02 48 89 d7 ff 50 20")) != 1:
        raise AssertionError(f"{path}: FIFO producer ring retain changed")
    fifo_free_start = value(FIFO_FREE)
    fifo_free_body = image[fifo_free_start:next_symbol(fifo_free_start)]
    if fifo_free_body.count(bytes.fromhex(
            "49 8b be 30 01 00 00 48 85 ff 74 06 48 8b 07 ff 50 28 "
            "49 c7 86 30 01 00 00 00 00 00 00")) != 1:
        raise AssertionError(f"{path}: FIFO producer ring final release changed")
    for anchor in ("41 89 d2 48 89 f2 48 8b 86 b8 00 00 00",
                   "44 8b 40 20", "48 81 c2 89 00 00 00",
                   "48 8b 80 30 01 00 00 44 8b 48 44 44 89 14 24"):
        if push_body.count(bytes.fromhex(anchor)) != 1:
            raise AssertionError(f"{path}: scheduler stamp/tail argument provenance changed")
    submit_start = value(RING_SUBMIT_TO_RING)
    submit_body = image[submit_start:next_symbol(submit_start)]
    # Every concrete native ring class shares this producer virtual. These are
    # typed receiver chains, not an untyped scan of every +0x138 virtual call.
    for table in (RING_VTABLE, "__ZTV24IGHardwareRingBufferBlit",
                  "__ZTV24IGHardwareRingBufferMain", "__ZTV25IGHardwareRingBufferMedia",
                  "__ZTV25IGHardwareRingBufferVEBox", "__ZTV27IGHardwareRingBufferCompute"):
        if struct.unpack_from("<Q", image, value(table) + 16 + 0x138)[0] != submit_start:
            raise AssertionError(f"{path}: native ring producer override changed: {table}")
    producer_chains = (
        (FIFO_SUBMIT_STAMP, "48 8b bb 30 01 00 00 48 8b 07 ff 90 38 01 00 00"),
        (FIFO_SUBMIT_COMMANDS, "49 8b bd 30 01 00 00 48 8b 07 ff 90 38 01 00 00"),
        (FIFO_SUBMIT_BUFFER, "49 8b bc 24 30 01 00 00 48 8b 07 ff 90 38 01 00 00"),
        (ACCEL_SUBMIT_SYNC, "49 8b 07 4c 89 ff ff 90 38 01 00 00"),
        (ACCEL_SUBMIT_MAIN, "48 8b 80 b8 00 00 00 48 8b b8 30 01 00 00 48 8b 07 "
                            "48 83 c4 08 5b 41 5e 41 5f 5d ff a0 38 01 00 00"),
    )
    for owner, anchor in producer_chains:
        start = value(owner)
        if image[start:next_symbol(start)].count(bytes.fromhex(anchor)) != 1:
            raise AssertionError(f"{path}: typed ring producer dispatch changed: {owner}")
    if len(direct_branches(ACCEL_SUBMIT_SYNC, FIFO_SUBMIT_STAMP)) != 2:
        raise AssertionError(f"{path}: sync producer stamp follow-up graph changed")
    sync_start = value(ACCEL_SUBMIT_SYNC)
    sync_body = image[sync_start:next_symbol(sync_start)]
    sync_ring = bytes.fromhex("4d 8b bc 24 30 01 00 00")
    sync_dispatch = bytes.fromhex("49 8b 07 4c 89 ff ff 90 38 01 00 00")
    if sync_body.count(sync_ring) != 1 or sync_body.index(sync_ring) >= sync_body.index(sync_dispatch) or \
            len(direct_branches(ACCEL_SUBMIT_SYNC, RING_WRITE_BUFFER)) != 3:
        raise AssertionError(f"{path}: sync FIFO/ring receiver or packet graph changed")
    for slot, method in (
            (0x138, "__ZN15IOAccelChannel219mergeEventExcludingEP12IOAccelEventS1_"),
            (0x140, "__ZN15IOAccelChannel213setEventStampEP12IOAccelEvent"),
            (0x148, "__ZN15IOAccelChannel214incrementStampEv")):
        address = value(FIFO_VTABLE) + 16 + slot
        if struct.unpack_from("<Q", image, address)[0] != 0 or \
                inherited_event_imports.get(address) != (method, 0x0E):
            raise AssertionError(f"{path}: FIFO inherited event/stamp virtual changed")
    scrub_slot = value("__ZTV16IntelAccelerator") + 16 + 0x8F0
    if struct.unpack_from("<Q", image, scrub_slot)[0] != 0 or \
            inherited_event_imports.get(scrub_slot) != ("__ZN22IOGraphicsAccelerator211scrubEventsEv", 0x0E):
        raise AssertionError(f"{path}: stamp rollover scrub virtual changed")
    for table, slot, method in (
            ("__ZTV13IGAccelShared", 0x128, "__ZN14IOAccelShared211scrubEventsEv"),
            ("__ZTV15IGAccelResource", 0x228, "__ZN16IOAccelResource211scrubEventsEv"),
            (EVENT_MACHINE_VTABLE, 0x270, "__ZN24IOAccelEventMachineFast210scrubEventEP12IOAccelEvent")):
        address = value(table) + 16 + slot
        if struct.unpack_from("<Q", image, address)[0] != 0 or \
                inherited_event_imports.get(address) != (method, 0x0E):
            raise AssertionError(f"{path}: concrete Intel scrub virtual changed: {table}")
    for anchor in ("44 8a 7b 48 45 84 ff", "c6 43 48 00",
                   "8b 53 64 8b 4b 68 45 31 c9", "45 0f b6 c7 ff 90 48 01 00 00"):
        if submit_body.count(bytes.fromhex(anchor)) != 1:
            raise AssertionError(f"{path}: per-submit stamp-presence/tail provenance changed")
    if not (submit_body.index(bytes.fromhex("44 8a 7b 48")) <
            submit_body.index(bytes.fromhex("c6 43 48 00")) <
            submit_body.index(bytes.fromhex("ff 90 48 01 00 00"))):
        raise AssertionError(f"{path}: stamp-presence capture/clear/dispatch order changed")
    # A false GuC/push result is NOT a recoverable rejection at the retained
    # ring caller. It branches past the success-only reset to a cold panic.
    result_anchor = bytes.fromhex(
        "ff 90 48 01 00 00 84 c0 74 14 c7 43 4c 00 00 00 00 "
        "c6 43 6c 00 5b 41 5c 41 5e 41 5f 5d c3 e8")
    if submit_body.count(result_anchor) != 1:
        raise AssertionError(f"{path}: native submit success/failure edge changed")
    failure_call = submit_start + submit_body.index(result_anchor) + len(result_anchor) - 1
    if failure_call + 5 + struct.unpack_from("<i", image, failure_call + 1)[0] != value(RING_SUBMIT_FAILURE):
        raise AssertionError(f"{path}: failed submission no longer reaches cold failure")
    failure_start = value(RING_SUBMIT_FAILURE)
    failure_body = image[failure_start:next_symbol(failure_start)]
    if failure_body != bytes.fromhex("55 48 89 e5 48 8d 3d a6 59 00 00 31 c0 e8 00 00 00 00") or \
            failure_start + 14 not in panic_relocations:
        raise AssertionError(f"{path}: native submit failure is not the verified panic import")
    message_start = failure_start + 11 + struct.unpack_from("<i", failure_body, 7)[0]
    message = image[message_start:image.index(0, message_start)]
    if message != b'"Enter debugger: submitToRing: Work queue failure detected"@tgl/IGHardwareRingBuffer.cpp:1762':
        raise AssertionError(f"{path}: native submit failure diagnosis changed")
    if struct.unpack_from("<Q", image,
            value(EVENT_MACHINE_VTABLE) + 16 + 0x220)[0] != timeout_start:
        raise AssertionError(f"{path}: inherited restart timeout override changed")
    timeout_body = image[timeout_start:next_symbol(timeout_start)]
    for slot_bytes, count, label in (
            (bytes.fromhex("ff 90 50 01 00 00"), 1, "checkForProgress"),
            (bytes.fromhex("ff 90 78 01 00 00"), 1, "pause"),
            (bytes.fromhex("ff 90 98 01 00 00"), 1, "getActiveContexts"),
            (bytes.fromhex("ff 90 80 01 00 00"), 2, "resume"),
            (bytes.fromhex("ff 90 88 01 00 00"), 1, "prepareGPUReset")):
        if timeout_body.count(slot_bytes) != count:
            raise AssertionError(
                f"{path}: eventTimeout {label} dispatch inventory changed")
    debug_calls = direct_branches(EVENT_TIMEOUT, ENCODE_DEBUG)
    halt_calls = direct_branches(EVENT_TIMEOUT, SCHEDULER_HALT)
    resume_calls = direct_branches(EVENT_TIMEOUT, SCHEDULER_RESUME)
    if len(debug_calls) != 3 or len(halt_calls) != 1 or len(resume_calls) != 2 or \
            not (debug_calls[0] < halt_calls[0] < resume_calls[0] <
                 resume_calls[1] < debug_calls[1] < debug_calls[2]):
        raise AssertionError(
            f"{path}: eventTimeout physical-helper inventory/order changed")

    # encodeDebugInfo is not a read-only formatter. It force-wakes and captures
    # eight legacy GuC scratch registers, then gathers each active engine. Ring
    # gathering calls getInstDoneSlice four times; that helper writes the global
    # 0xFDC selector for six slices before restoring it. Pin the complete direct
    # graph and the destructive register inventory behind the VF route.
    if len(direct_branches(ENCODE_DEBUG, GATHER_GUC)) != 1 or \
            len(direct_branches(ENCODE_DEBUG, GATHER_RING)) != 1:
        raise AssertionError(f"{path}: debug-capture call graph changed")
    gather_guc_start = value(GATHER_GUC)
    gather_guc_body = image[gather_guc_start:next_symbol(gather_guc_start)]
    if len(direct_branches(GATHER_GUC, SAFE_FORCE_WAKE)) != 2 or any(
            gather_guc_body.count(struct.pack("<I", register)) != 1
            for register in range(0xC184, 0xC1A1, 4)):
        raise AssertionError(f"{path}: GuC scratch debug capture changed")
    if len(direct_branches(GATHER_RING, SAFE_FORCE_WAKE)) != 5 or \
            len(direct_branches(GATHER_RING, GET_INSTDONE)) != 4:
        raise AssertionError(f"{path}: ring debug-capture graph changed")
    instdone_start = value(GET_INSTDONE)
    instdone_body = image[instdone_start:next_symbol(instdone_start)]
    if instdone_body.count(struct.pack("<I", 0xFDC)) != 4 or any(
            instdone_body.count(struct.pack("<I", register)) != 1
            for register in (0x7100, 0xE160, 0xE164)) or \
            instdone_body.count(bytes.fromhex("48 81 ff 00 00 00 06")) != 1:
        raise AssertionError(
            f"{path}: destructive INSTDONE selector protocol changed")

    for owner, store in (
            (SCHEDULER_HALT, bytes.fromhex("42 c7 04 20 01 00 01 00")),
            (SCHEDULER_RESUME, bytes.fromhex("42 c7 04 20 00 00 01 00"))):
        start = value(owner)
        body = image[start:next_symbol(start)]
        if len(direct_branches(owner, SAFE_FORCE_WAKE)) != 2 or \
                body.count(bytes.fromhex("48 8b 87 40 12 00 00")) != 2 or \
                body.count(store) != 1 or \
                body.count(bytes.fromhex("bb 11 27 00 00")) != 1:
            raise AssertionError(
                f"{path}: physical timeout halt/resume protocol changed: {owner}")

    # IOAccel can request the same physical capture independently of the event
    # timeout. Both exported roots call doHangAnalysis and dumpHangAnalysis; the
    # latter reaches raw ring-status and full RCS register dumps through the
    # ring buffer's cached MMIO base at +0x50. Route both shared boundaries so a
    # diagnostic request cannot become a PF register probe on a VF.
    for owner in (RING_DEBUG_ENGINE, FIFO_DIAGNOSIS):
        if len(direct_branches(owner, RING_DO_HANG)) != 1 or \
                len(direct_branches(owner, RING_DUMP_HANG)) != 1:
            raise AssertionError(
                f"{path}: hardware-diagnosis root graph changed: {owner}")
    if len(direct_branches(RING_DO_HANG, GATHER_RING)) != 1 or \
            len(direct_branches(RING_DUMP_HANG, RING_DUMP_STATUS)) != 1 or \
            len(direct_branches(RING_DUMP_HANG, RING_DUMP_REGISTERS)) != 1:
        raise AssertionError(f"{path}: physical hang-diagnosis graph changed")
    if len(direct_branches(RING_DUMP_STATUS, SAFE_FORCE_WAKE)) != 2 or \
            len(direct_branches(RING_DUMP_REGISTERS, SAFE_FORCE_WAKE)) != 2:
        raise AssertionError(f"{path}: hang dump force-wake inventory changed")
    rcs_dump_start = value(RING_DUMP_REGISTERS_RCS)
    rcs_dump_body = image[rcs_dump_start:next_symbol(rcs_dump_start)]
    if rcs_dump_body.count(bytes.fromhex(
            "48 8b 43 50 44 8b 80 28 20 00 00")) != 1:
        raise AssertionError(f"{path}: raw RCS register-dump anchor changed")

    # IOAccel's FIFO reset virtual is a second error-recovery root. It invokes
    # the ring-buffer +0x168 reset virtual, resumes the scheduler and replays up
    # to two stamps. The concrete physical reset takes five force-wake paths,
    # writes engine-control registers, executes the 0x4A08/0x941C/0xCEC4 reset
    # sequence and replays the accelerator reset list. Both the root and the
    # lower primitive must remain behind classified-VF routes.
    ring_vtable = value(RING_VTABLE)
    fifo_vtable = value(FIFO_VTABLE)
    if struct.unpack_from(
            "<Q", image, ring_vtable + 16 + 0x168)[0] != value(
                RING_RESET_GRAPHICS):
        raise AssertionError(f"{path}: ring physical-reset virtual slot changed")
    if struct.unpack_from(
            "<Q", image, fifo_vtable + 16 + 0x200)[0] != value(
                FIFO_RESET_REPLAY):
        raise AssertionError(f"{path}: FIFO reset/replay virtual slot changed")
    replay_start = value(FIFO_RESET_REPLAY)
    replay_body = image[replay_start:next_symbol(replay_start)]
    for dispatch, count, label in (
            (bytes.fromhex("ff 90 68 01 00 00"), 1, "physical reset"),
            (bytes.fromhex("ff 90 80 01 00 00"), 2, "scheduler resume"),
            (bytes.fromhex("ff 90 d8 01 00 00"), 2, "stamp completion")):
        if replay_body.count(dispatch) != count:
            raise AssertionError(
                f"{path}: FIFO reset/replay {label} inventory changed")
    if len(direct_branches(FIFO_RESET_REPLAY, FIFO_SUBMIT_STAMP)) != 2 or \
            len(direct_branches(FIFO_RESET_REPLAY, RING_SLEEP_STAMP)) != 2:
        raise AssertionError(f"{path}: FIFO reset/replay submission graph changed")
    reset_start = value(RING_RESET_GRAPHICS)
    reset_body = image[reset_start:next_symbol(reset_start)]
    if len(direct_branches(RING_RESET_GRAPHICS, SAFE_FORCE_WAKE)) != 4 or \
            len(direct_branches(RING_RESET_GRAPHICS, SAFE_FORCE_WAKE_BOOL)) != 1 or \
            len(direct_branches(RING_RESET_GRAPHICS, GET_DEFAULT_RESET)) != 1:
        raise AssertionError(f"{path}: physical engine-reset call graph changed")
    for anchor, count, label in (
            (bytes.fromhex("48 8b 80 40 12 00 00"), 2, "raw MMIO base"),
            (bytes.fromhex("c7 80 08 4a 00 00 04 00 04 00"), 1,
             "engine reset assert"),
            (bytes.fromhex("c7 80 08 4a 00 00 00 00 04 00"), 1,
             "engine reset release"),
            (bytes.fromhex("c7 80 00 41 00 00 b1 b1 f0 f0"), 1,
             "fault register clear 0"),
            (bytes.fromhex("c7 80 04 41 00 00 b2 b2 f0 f0"), 1,
             "fault register clear 1"),
            (bytes.fromhex("ff 90 90 01 00 00"), 1,
             "scheduler reset completion")):
        if reset_body.count(anchor) != count:
            raise AssertionError(
                f"{path}: physical engine-reset {label} inventory changed")

    # Context shared-private lifecycle reaches routed GuC descriptor operations,
    # not the similarly named IGGuC shared-private allocation methods. Cleanup
    # invalidates translations before detaching the descriptor.
    if len(direct_branches(SCHEDULER4_INIT_PRIVATE, GUC_ATTACH_DESC)) != 1:
        raise AssertionError(f"{path}: shared-private attach dispatch changed")
    invalidate_edges = direct_branches(SCHEDULER4_CLEANUP_PRIVATE, GUC_INVALIDATE_TLB)
    detach_edges = direct_branches(SCHEDULER4_CLEANUP_PRIVATE, GUC_DETACH_DESC)
    if len(invalidate_edges) != 1 or len(detach_edges) != 1 or \
            invalidate_edges[0] >= detach_edges[0]:
        raise AssertionError(f"{path}: shared-private invalidate/detach order changed")
    for slot, target in ((0x128, SCHEDULER4_INIT_PRIVATE),
                         (0x138, SCHEDULER4_CLEANUP_PRIVATE),
                         (0x1c8, SCHEDULER4_BIND),
                         (0x1d8, SCHEDULER4_UNBIND)):
        if struct.unpack_from("<Q", image,
                              value(SCHEDULER4_VTABLE) + 16 + slot)[0] != value(target):
            raise AssertionError(f"{path}: context/ring ownership slot {slot:#x} changed")
    context_free_start = value(CONTEXT_FREE)
    context_free_body = image[context_free_start:next_symbol(context_free_start)]
    ownership_anchors = (
        bytes.fromhex("48 8b bb b0 00 00 00"),
        bytes.fromhex("48 8b bb b8 00 00 00"),
        bytes.fromhex("ff 90 38 01 00 00"),
        bytes.fromhex("48 8b bb a8 00 00 00"),
        bytes.fromhex("48 8b b3 98 00 00 00"),
        bytes.fromhex("48 8b 7b 58"))
    if [context_free_body.count(anchor) for anchor in ownership_anchors] != [1, 1, 1, 1, 2, 1] or \
            [context_free_body.index(anchor) for anchor in ownership_anchors] != sorted(
                context_free_body.index(anchor) for anchor in ownership_anchors):
        raise AssertionError(f"{path}: context detach/ring/image/task teardown order changed")
    address_start = value(CONTEXT_RING_GPU_ADDRESS)
    address_body = image[address_start:next_symbol(address_start)]
    if address_body.count(bytes.fromhex(
            "48 8b 87 b0 00 00 00 48 8b b8 80 00 00 00")) != 1:
        raise AssertionError(f"{path}: context DMA ring backing source changed")
    init_start = value(CONTEXT_INIT)
    init_body = image[init_start:next_symbol(init_start)]
    if init_body.count(bytes.fromhex(
            "ff 90 28 01 00 00 49 8b 7d 50")) != 1:
        raise AssertionError(f"{path}: native unchecked context attach call changed")

    # Both stamp address views borrow the task's same +0x288 buffer. Ordinary
    # contexts retain the task, while the flag-bit-0 path skips that retain;
    # the task's last-release path attempts notification of four owned contexts.
    # These local contracts do not prove external task ownership or completion.
    for getter, target in ((TASK_STAMP_GPU_ADDRESS, MAPPED_BUFFER_GPU_ADDRESS),
                           (TASK_STAMPS, SHARED_BUFFER_CPU_ADDRESS)):
        getter_start = value(getter)
        getter_body = image[getter_start:next_symbol(getter_start)]
        if getter_body[:12] != bytes.fromhex("55 48 89 e5 48 8b bf 88 02 00 00 5d") or \
                len(direct_branches(getter, target)) != 1:
            raise AssertionError(f"{path}: task stamp CPU/GPU backing identity changed")
    scratch_getter_start = value(TASK_SCRATCH_GPU_ADDRESS)
    scratch_getter_body = image[scratch_getter_start:next_symbol(scratch_getter_start)]
    if scratch_getter_body[:12] != bytes.fromhex("55 48 89 e5 48 8b bf 80 02 00 00 5d") or \
            len(direct_branches(TASK_SCRATCH_GPU_ADDRESS, MAPPED_BUFFER_GPU_ADDRESS)) != 1:
        raise AssertionError(f"{path}: task scratch backing provenance changed")
    if init_body.count(bytes.fromhex(
            "41 f6 45 6e 01 75 12 49 8b 7d 58 48 8b 07 ff 50 20")) != 1:
        raise AssertionError(f"{path}: context conditional task retain changed")
    stamp_init_start = value(TASK_INIT_STAMPS)
    stamp_init_body = image[stamp_init_start:next_symbol(stamp_init_start)]
    if len(direct_branches(TASK_INIT_STAMPS, SHARED_BUFFER_CLONE)) != 1 or \
            len(direct_branches(TASK_INIT_STAMPS, SHARED_BUFFER_FACTORY)) != 1 or \
            stamp_init_body.count(bytes.fromhex("be 00 30 00 00")) != 1 or \
            stamp_init_body.count(bytes.fromhex("48 89 83 88 02 00 00")) != 1:
        raise AssertionError(f"{path}: task stamp allocation/clone contract changed")
    task_free_start = value(TASK_FREE)
    task_free_body = image[task_free_start:next_symbol(task_free_start)]
    if task_free_body.count(bytes.fromhex("48 8b bb 88 02 00 00")) != 1 or \
            task_free_body.count(bytes.fromhex("48 c7 83 88 02 00 00 00 00 00 00")) != 1:
        raise AssertionError(f"{path}: task stamp final-release contract changed")
    release_start = value(TASK_RELEASE)
    release_body = image[release_start:next_symbol(release_start)]
    if len(direct_branches(TASK_RELEASE, CONTEXT_NOTIFY_COMPLETE)) != 4:
        raise AssertionError(f"{path}: task owned-context completion inventory changed")
    for offset in (0x2a0, 0x2a8, 0x298, 0x290):
        if release_body.count(b"\x48\xc7\x83" + struct.pack("<I", offset) + b"\0" * 4) != 1:
            raise AssertionError(f"{path}: task owned-context {offset:#x} cleanup changed")
    if len(direct_branches(CONTEXT_NOTIFY_COMPLETE, FIFO_NOTIFY_COMPLETE)) != 1 or \
            len(direct_branches(FIFO_NOTIFY_COMPLETE, RING_NOTIFY_COMPLETE)) != 1:
        raise AssertionError(f"{path}: task/context/FIFO notification graph changed")
    notify_start = value(CONTEXT_NOTIFY_COMPLETE)
    notify_body = image[notify_start:next_symbol(notify_start)]
    if notify_body.count(bytes.fromhex(
            "4d 85 ff 74 16 48 8b 7b 58 48 8b 07 ff 50 20 c6 83 c8 00 00 00 01")) != 1:
        raise AssertionError(f"{path}: asynchronous context notification task retain changed")
    # notifyComplete registers an event dependency, not GPU completion. The
    # inherited mergeEvent virtual is unresolved on disk: prove its external
    # relocation rather than interpreting the zero vtable word as a local call.
    merge_slot = value(EVENT_MACHINE_VTABLE) + 16 + 0x1b8
    merge_relocations = []
    for index in range(external_count):
        address, bits = struct.unpack_from("<iI", image, external_offset + index * 8)
        if address == merge_slot:
            merge_relocations.append((names[bits & 0xffffff],
                ((bits >> 24) & 1, (bits >> 25) & 3,
                 (bits >> 27) & 1, (bits >> 28) & 0xf)))
    if merge_relocations != [(EVENT_MERGE, (0, 3, 1, 0))] or \
            struct.unpack_from("<Q", image, merge_slot)[0] != 0:
        raise AssertionError(f"{path}: ring notification mergeEvent virtual changed")
    # The mapped-buffer getter delegates to an inherited mapping method. Its
    # unresolved vtable word is NOT a GGTT address or proof of address space.
    mapping_slot = value(MEMORY_MAP_VTABLE) + 16 + 0x128
    mapping_init = value(MAPPED_BUFFER_INIT)
    expected_mapping_relocations = {
        mapping_slot: (MEMORY_MAP_GPU_ADDRESS, (0, 3, 1, 0)),
        mapping_init + 0x71: (SYS_MEMORY_FACTORY, (1, 2, 1, 2)),
        mapping_init + 0xca: (PREPARE_MAPPING, (1, 2, 1, 2)),
        value(MEMORY_MAP_VTABLE) + 16 + 0x140:
            (MEMORY_MAP_COMPLETE, (0, 3, 1, 0)),
        value(MAPPED_BUFFER_FREE) + 0x13:
            (MEMORY_MAP_FINISH_EVENT, (1, 2, 1, 2)),
        value(SHARED_BUFFER_FREE) + 0x23:
            (SYS_MEMORY_UNLOCK, (1, 2, 1, 2)),
        value(SHARED_BUFFER_UNLOCK) + 0x1c:
            (SYS_MEMORY_UNLOCK, (1, 2, 1, 2)),
    }
    observed_mapping_relocations = {address: [] for address in expected_mapping_relocations}
    for index in range(external_count):
        address, bits = struct.unpack_from("<iI", image, external_offset + index * 8)
        if address in observed_mapping_relocations:
            observed_mapping_relocations[address].append((names[bits & 0xffffff],
                ((bits >> 24) & 1, (bits >> 25) & 3,
                 (bits >> 27) & 1, (bits >> 28) & 0xf)))
    for address, expected in expected_mapping_relocations.items():
        if observed_mapping_relocations[address] != [expected]:
            raise AssertionError(f"{path}: mapping provenance relocation {address:#x} changed")
    if struct.unpack_from("<Q", image, mapping_slot)[0] != 0:
        raise AssertionError(f"{path}: inherited mapping getter unexpectedly resolved")
    free_start = value(MAPPED_BUFFER_FREE)
    free_body = image[free_start:next_symbol(free_start)]
    for anchor in ("48 8b 7b 30 48 8b 07 ff 90 40 01 00 00",
                   "ff 50 28 48 c7 43 30 00 00 00 00"):
        if free_body.count(bytes.fromhex(anchor)) != 1:
            raise AssertionError(f"{path}: mapped-buffer mapping teardown changed")
    # Explicit task cleanup only drops references, whereas CPU unlock clears
    # its mapping even on a retained object. Neither is a GPU idle proof.
    stamps_start = value(TASK_RELEASE_STAMPS)
    stamps_body = image[stamps_start:next_symbol(stamps_start)]
    if stamps_body.count(bytes.fromhex("ff 50 28")) != 2:
        raise AssertionError(f"{path}: explicit task stamp cleanup releases changed")
    for offset in (0x280, 0x288):
        if stamps_body.count(b"\x48\xc7\x83" + struct.pack("<I", offset) + b"\0" * 4) != 1:
            raise AssertionError(f"{path}: explicit task stamp cleanup {offset:#x} changed")
    unlock_start = value(SHARED_BUFFER_UNLOCK)
    unlock_body = image[unlock_start:next_symbol(unlock_start)]
    if unlock_body.count(bytes.fromhex("48 c7 43 38 00 00 00 00")) != 1 or \
            bytes.fromhex("ff 90 40 01 00 00") in unlock_body:
        raise AssertionError(f"{path}: CPU unlock/GPU mapping distinction changed")
    for table, destructor in ((MAPPED_BUFFER_VTABLE, MAPPED_BUFFER_FREE),
                              (SHARED_BUFFER_VTABLE, SHARED_BUFFER_FREE)):
        if struct.unpack_from("<Q", image, value(table) + 16 + 0x90)[0] != value(destructor):
            raise AssertionError(f"{path}: backing destructor virtual changed")
    # These concrete helper methods have no entry in ANY local vtable. This
    # excludes local virtual dispatch to them, not inherited CPU-unlock calls
    # or externally linked direct calls to their exported symbols.
    explicit_cleanup_targets = {value(SHARED_BUFFER_UNLOCK), value(TASK_RELEASE_STAMPS)}
    for name, table_start in zip(names, values):
        if not name.startswith("__ZTV") or not table_start:
            continue
        table_end = next_symbol(table_start)
        for slot in range(table_start + 16, table_end - 7, 8):
            if struct.unpack_from("<Q", image, slot)[0] in explicit_cleanup_targets:
                raise AssertionError(f"{path}: explicit mapping cleanup became virtual in {name}")
    mapping_body = image[mapping_init:next_symbol(mapping_init)]
    for anchor in ("ff 91 38 01 00 00", "ff 90 38 01 00 00",
                   "84 c0 74 2d 4c 89 7b 30 4c 89 73 10 4c 89 6b 18"):
        if mapping_body.count(bytes.fromhex(anchor)) != 1:
            raise AssertionError(f"{path}: mapping admission/publication contract changed")
    options_start = value(MAPPED_BUFFER_MAPPING_OPTIONS)
    if image[options_start:options_start + 11] != bytes.fromhex(
            "55 48 89 e5 b8 07 00 00 00 5d c3"):
        raise AssertionError(f"{path}: mapped-buffer mapping options changed")
    config_start = value(POPULATE_ACCEL_CONFIG)
    config_body = image[config_start:next_symbol(config_start)]
    # Pin the RIP-relative PPGTT property lookup, default 1, and the bit-8
    # assignment in the 64-bit feature word. The inherited option-bit meaning
    # is still unknown; do not turn this observation into a GGTT quota check.
    ppgtt_lookup = config_start + 0xe2
    if image[ppgtt_lookup:ppgtt_lookup + 3] != bytes.fromhex("48 8d 35"):
        raise AssertionError(f"{path}: PPGTT property lookup changed")
    ppgtt_name = ppgtt_lookup + 7 + struct.unpack_from("<i", image, ppgtt_lookup + 3)[0]
    if image[ppgtt_name:ppgtt_name + 6] != b"PPGTT\0" or \
            image[ppgtt_lookup + 7:ppgtt_lookup + 15] != bytes.fromhex(
                "4c 89 e7 ba 01 00 00 00"):
        raise AssertionError(f"{path}: PPGTT property/default changed")
    if config_body.count(bytes.fromhex(
            "21 d8 c1 e0 08 48 c7 c1 ff fe ff ff 49 23 8c 24 90 11 00 00 "
            "48 09 c1 49 89 8c 24 90 11 00 00")) != 1:
        raise AssertionError(f"{path}: PPGTT feature-bit publication changed")
    ring_notify_start = value(RING_NOTIFY_COMPLETE)
    ring_notify_body = image[ring_notify_start:next_symbol(ring_notify_start)]
    for anchor in (bytes.fromhex("83 7f 38 00 78 30"),
                   bytes.fromhex("b3 01 48 85 f6 74 28"),
                   bytes.fromhex("ff 90 b8 01 00 00")):
        if ring_notify_body.count(anchor) != 1:
            raise AssertionError(f"{path}: ring dependency-registration contract changed")
    if direct_branches(RING_NOTIFY_COMPLETE, RING_SLEEP_STAMP) or \
            direct_branches(RING_NOTIFY_COMPLETE, SAFE_FORCE_WAKE):
        raise AssertionError(f"{path}: ring notification gained a hardware wait")

    # Normal garbage collection checks the concrete context idle virtual.
    # Forced collection/drain intentionally bypass that check; descriptor
    # deregistration and retained DMA backing must remain independent barriers.
    for table, slot, target in ((CONTEXT_VTABLE, 0x128, CONTEXT_CHECK),
                                (CONTEXT_VTABLE, 0x118, GC_OBJECT_RELEASE_UNCHECKED),
                                (SCHEDULER4_VTABLE, 0x168, SCHEDULER4_CONTEXT_IDLE)):
        if struct.unpack_from("<Q", image, value(table) + 16 + slot)[0] != value(target):
            raise AssertionError(f"{path}: garbage collection virtual {slot:#x} changed")
    if len(direct_branches(GC_OBJECT_RELEASE, GC_ADD)) != 1 or \
            len(direct_branches(SCHEDULER4_CONTEXT_IDLE, GUC_KMD_CONTEXT_IDLE)) != 1:
        raise AssertionError(f"{path}: context GC/idle dispatch graph changed")
    for owner, anchor, count in (
            (GC_OBJECT_RELEASE, bytes.fromhex("ff 90 28 01 00 00"), 1),
            (CONTEXT_CHECK, bytes.fromhex("48 8b 80 68 01 00 00"), 1),
            (GC_COLLECT, bytes.fromhex("ff 50 28"), 1),
            (GC_FORCE_COLLECT, bytes.fromhex("ff 90 18 01 00 00"), 1),
            (GC_DRAIN, bytes.fromhex("ff 50 28"), 1),
            (GC_DRAIN, bytes.fromhex("ff 90 18 01 00 00"), 1)):
        owner_start = value(owner)
        if image[owner_start:next_symbol(owner_start)].count(anchor) != count:
            raise AssertionError(f"{path}: context GC release/check inventory changed in {owner}")

    # Normal producer backpressure polls shared context head/stamp memory.
    # Keep its timeout diagnostic behind the already isolated entry rather
    # than replacing normal waits or manufacturing completion.
    if len(direct_branches(RING_WAIT_SPACE, RING_WAIT_TIMEOUT)) != 2 or \
            len(direct_branches(RING_WAIT_TIMEOUT, RING_DEBUG_ENGINE)) != 1:
        raise AssertionError(f"{path}: ring backpressure timeout graph changed")
    wait_start = value(RING_WAIT_TIMEOUT)
    wait_body = image[wait_start:next_symbol(wait_start)]
    stamp_start = value(RING_SLEEP_STAMP)
    stamp_body = image[stamp_start:next_symbol(stamp_start)]
    # Slot +8 is CPU-published submitted bookkeeping, not the completed value
    # read at slot +0. Main/compute override the packet emitter but retain the
    # same software stamp bookkeeping. Never use the +8 write as GPU evidence.
    for owner in (RING_WRITE_STAMP, RING_MAIN_WRITE_STAMP, RING_COMPUTE_WRITE_STAMP):
        owner_start = value(owner)
        body = image[owner_start:next_symbol(owner_start)]
        for anchor in (bytes.fromhex("ff 90 40 01 00 00"),
                       bytes.fromhex("44 89 74 08 08 44 89 73 44 c6 43 48 01")):
            if body.count(anchor) != 1:
                raise AssertionError(f"{path}: submitted-stamp bookkeeping changed in {owner}")
    if stamp_body.count(bytes.fromhex(
            "48 c1 e2 06 8b 04 10 41 89 07")) != 1:
        raise AssertionError(f"{path}: completed-stamp slot-zero read changed")
    for table, commit in ((RING_VTABLE, RING_COMMIT_STAMP),
                          ("__ZTV24IGHardwareRingBufferMain", RING_MAIN_COMMIT_STAMP),
                          ("__ZTV27IGHardwareRingBufferCompute", RING_COMPUTE_COMMIT_STAMP)):
        if struct.unpack_from("<Q", image, value(table) + 16 + 0x140)[0] != value(commit):
            raise AssertionError(f"{path}: stamp packet encoder virtual changed")
    # Main/compute encode PIPE_CONTROL post-sync writes. Pin the destination
    # (stamp GPU base + index*64), sequence data, scratch prerequisite and
    # actual writeBuffer graph; source bookkeeping alone cannot prove execution.
    for owner in (RING_MAIN_COMMIT_STAMP, RING_COMPUTE_COMMIT_STAMP):
        start = value(owner)
        body = image[start:next_symbol(start)]
        if len(direct_branches(owner, RING_WRITE_BUFFER)) != 4 or \
                len(direct_branches(owner, TASK_SCRATCH_GPU_ADDRESS)) != 2 or \
                len(direct_branches(owner, RING_GTT_WRITE_MODE)) != 2:
            raise AssertionError(f"{path}: PIPE_CONTROL stamp encoder call graph changed")
        for anchor in (bytes.fromhex("48 b8 04 00 00 7a 98 44 10 01"),
                       bytes.fromhex("49 63 74 24 38 48 c1 e6 06 49 03 74 24 28"),
                       bytes.fromhex("8b 55 b4 49 89 55 10")):
            if body.count(anchor) != 1:
                raise AssertionError(f"{path}: PIPE_CONTROL stamp packet/data anchor changed")
    base_start = value(RING_COMMIT_STAMP)
    base_body = image[base_start:next_symbol(base_start)]
    if len(direct_branches(RING_COMMIT_STAMP, RING_WRITE_BUFFER)) != 3 or \
            base_body.count(bytes.fromhex("48 8d 84 02 03 40 00 13")) != 1:
        raise AssertionError(f"{path}: base MI_FLUSH_DW stamp encoder changed")
    for body, anchor, count, label in (
            (wait_body, bytes.fromhex("48 8b 43 18 8b 40 10"), 1,
             "shared context head"),
            (wait_body, bytes.fromhex("ff 90 50 01 00 00"), 1,
             "scheduler progress query"),
            (stamp_body, bytes.fromhex("48 8b 43 30"), 3,
             "shared stamp backing")):
        if body.count(anchor) != count:
            raise AssertionError(f"{path}: ring wait {label} inventory changed")
    for owner in (RING_WAIT_SPACE, RING_WAIT_TIMEOUT, RING_SLEEP_STAMP):
        if direct_branches(owner, SAFE_FORCE_WAKE) or \
                direct_branches(owner, RING_RESET_GRAPHICS):
            raise AssertionError(f"{path}: normal ring wait enters physical recovery")

    dpsm_start = value(DPSM_IDLE_TIMER)
    dpsm_end = next_symbol(dpsm_start)
    if image[dpsm_start:dpsm_end].count(DPSM_SCHEDULER_IDLE_SLOT) != 1 or \
            image[dpsm_start:dpsm_end].count(DPSM_NOTIFY_SLOT) != 1:
        raise AssertionError(f"{path}: DPSM timer dispatch contract changed")
    notify_start = value(DPSM_NOTIFY)
    notify_end = next_symbol(notify_start)
    if image[notify_start:notify_end] != DPSM_NOTIFY_BODY:
        raise AssertionError(f"{path}: DPSM notification is no longer an exact no-op")
    callback_start = value(INIT_LOCAL_CALLBACKS)
    callback_end = next_symbol(callback_start)
    callback_refs = []
    for candidate in range(callback_start, callback_end - 10):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 0d") or \
                image[candidate + 7:candidate + 11] != bytes.fromhex("48 89 48 38"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == notify_start:
            callback_refs.append(candidate)
    if len(callback_refs) != 1:
        raise AssertionError(f"{path}: local callback table no longer pins DPSM no-op")
    coarse_start = value(ENABLE_COARSE_POWER_GATING)
    coarse_end = next_symbol(coarse_start)
    if image[coarse_start:coarse_end] != VOID_NOOP_BODY:
        raise AssertionError(f"{path}: coarse-power callback is no longer an exact no-op")
    coarse_refs = []
    for candidate in range(callback_start, callback_end - 9):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 0d") or \
                image[candidate + 7:candidate + 10] != bytes.fromhex("48 89 08"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == coarse_start:
            coarse_refs.append(candidate)
    if len(coarse_refs) != 1 or \
            image[accelerator_start:accelerator_start_end].count(
                DPSM_COARSE_POWER_SLOT) != 1:
        raise AssertionError(f"{path}: native start coarse-power no-op dispatch changed")

    # Headless registration installs a local 0x60-byte callback table instead
    # of a framebuffer provider. Pin every initialized entry and its exact
    # software-only body, not only the two callbacks reached during start.
    local_callbacks = (
        (0x00, ENABLE_COARSE_POWER_GATING, VOID_NOOP_BODY),
        (0x08, LOCAL_SAFE_FORCE_WAKE, VOID_NOOP_BODY),
        (0x10, LOCAL_PAVP_CONTROL, VOID_NOOP_BODY),
        (0x20, LOCAL_MEDIA_LOAD, VOID_NOOP_BODY),
        (0x28, LOCAL_MEDIA_PREPARE, VOID_NOOP_BODY),
        (0x30, LOCAL_CLIENT_NOTIFY, VOID_NOOP_BODY),
        (0x38, DPSM_NOTIFY, ZERO_NOOP_BODY),
        (0x40, LOCAL_PM_NOTIFY, ZERO_NOOP_BODY),
        (0x48, LOCAL_GUC_WILL_LOAD, UNSUPPORTED_NOOP_BODY),
        (0x50, LOCAL_GUC_FAILED, VOID_NOOP_BODY),
        (0x58, LOCAL_GUC_DID_LOAD, VOID_NOOP_BODY),
    )
    for slot, name, expected_body in local_callbacks:
        target = value(name)
        if image[target:next_symbol(target)].rstrip(b"\x90") != expected_body:
            raise AssertionError(
                f"{path}: local callback {name} is no longer software-only")
        refs = []
        for candidate in range(callback_start, callback_end - 10):
            if image[candidate:candidate + 3] != bytes.fromhex("48 8d 0d"):
                continue
            displacement = struct.unpack_from("<i", image, candidate + 3)[0]
            if candidate + 7 + displacement != target:
                continue
            store = image[candidate + 7:candidate + 11]
            expected_store = (bytes.fromhex("48 89 08") if slot == 0 else
                              bytes((0x48, 0x89, 0x48, slot)))
            if store.startswith(expected_store):
                refs.append(candidate)
        if len(refs) != 1:
            raise AssertionError(
                f"{path}: local callback slot {slot:#x} no longer pins {name}")

    # Guest sleep/wake reaches the same routed engine boundaries. Scheduler
    # firmware initialization is explicitly idempotent: its +0x20 loaded byte
    # bypasses the +0x220 loadFirmware virtual call on every wake after the
    # first successful GuC/CTB construction. The bridge sleep/wake helpers
    # continue to converge on the already audited disable/enable methods.
    for target in (BRIDGE_SYSTEM_SLEEP, BRIDGE_SYSTEM_WAKE, START, STOP):
        if len(direct_branches(SET_POWER_STATE, target)) != 1:
            raise AssertionError(
                f"{path}: accelerator power-state edge to {target} changed")
    if len(direct_branches(BRIDGE_SYSTEM_SLEEP, BRIDGE_DISABLE)) != 1 or \
            len(direct_branches(BRIDGE_SYSTEM_WAKE, BRIDGE_ENABLE)) != 1:
        raise AssertionError(
            f"{path}: interrupt-bridge sleep/wake lifecycle changed")
    intel_sleep = "__ZN16IntelAccelerator15systemWillSleepEv"
    assert direct_branches(intel_sleep, "__ZN18IGStolenMemoryPool5purgeEv") == [0x2894b], f"{path}: changed pre-base stolen-pool purge"
    assert direct_branches(intel_sleep, BRIDGE_SYSTEM_SLEEP) == [0x28967], f"{path}: changed post-base bridge sleep"
    purge_slot = value("__ZTV24IGStolenMemoryDescriptor") + 16 + 0x120
    assert struct.unpack_from("<Q", image, purge_slot)[0] == value("__ZN24IGStolenMemoryDescriptor12setPurgeableEjPj"), f"{path}: changed declared stolen purgeable target"
    assert direct_branches("__ZN18IGStolenMemoryPool8allocateEm", "__ZN24IGStolenMemoryDescriptor12withSubRangeEP18IGStolenMemoryPoolP18IOMemoryDescriptoryyj") == [0xc8bd], f"{path}: changed stolen descriptor factory edge"
    firmware_start = value(SCHEDULER_INIT_FIRMWARE)
    firmware_body = image[firmware_start:next_symbol(firmware_start)]
    firmware_steps = (
        bytes.fromhex("f6 47 0c 01"),
        bytes.fromhex("80 7f 20 00"),
        bytes.fromhex("ff 90 20 02 00 00"),
        bytes.fromhex("c6 43 20 01"),
    )
    positions = [firmware_body.find(step) for step in firmware_steps]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError(
            f"{path}: scheduler firmware idempotence guard changed")

    # Accelerator feature +0x1190 bit 5 makes native start call this routine
    # before startGraphicsEngine. It writes raw MMIO 0xA204 under physical
    # force-wake, so source admission must reject that mode on a VF.
    async_calls = direct_branches(ACCELERATOR_START, SET_ASYNC_SLICE_COUNT)
    if len(async_calls) != 1:
        raise AssertionError(f"{path}: native async-slice start edge changed")
    async_call = async_calls[0]
    predicates = []
    cursor = accelerator_start
    while True:
        cursor = image.find(bytes.fromhex("41 f6 06 20 74"), cursor,
                            async_call)
        if cursor < 0:
            break
        skip = struct.unpack_from("<b", image, cursor + 5)[0]
        if cursor + 6 + skip == async_call + 5:
            predicates.append(cursor)
        cursor += 1
    if len(predicates) != 1:
        raise AssertionError(
            f"{path}: async-slice call is not gated by +0x1190 bit 5")
    async_start = value(SET_ASYNC_SLICE_COUNT)
    async_end = next_symbol(async_start)
    if image[async_start:async_end].count(ASYNC_SLICE_MMIO_ANCHOR) != 1:
        raise AssertionError(f"{path}: async-slice physical-MMIO anchor changed")
    engine_start_calls = direct_branches(ACCELERATOR_START, START)
    hws_init_calls = direct_branches(ACCELERATOR_START, INIT_HARDWARE_STATUS_MEMORY)
    if len(engine_start_calls) != 1 or len(hws_init_calls) != 1 or \
            len(direct_branches(INIT_HARDWARE_STATUS_MEMORY,
                                MAPPED_WITH_OPTIONS)) != 2:
        raise AssertionError(f"{path}: native start/HWS mapped-buffer graph changed")

    # The original engine-start body is intentionally never entered on a VF.
    # Pin representative physical descendants so that this safety boundary is
    # explicit: it acquires force-wake, initializes mode registers and writes
    # every engine/global HWS address through the accelerator's raw MMIO base.
    for target in (SAFE_FORCE_WAKE, INIT_MODE_REGISTERS,
                   INIT_HARDWARE_STATUS_REGISTERS):
        if not direct_branches(START, target):
            raise AssertionError(
                f"{path}: physical engine-start edge {START} -> {target} changed")
    hws_register_start = value(INIT_HARDWARE_STATUS_REGISTERS)
    hws_register_body = image[
        hws_register_start:next_symbol(hws_register_start)]
    if hws_register_body.count(HWS_ENGINE_MMIO_ANCHOR) != 1 or \
            hws_register_body.count(HWS_GLOBAL_MMIO_ANCHOR) != 1:
        raise AssertionError(
            f"{path}: physical HWS-register MMIO inventory changed")

    # Tiled/aperture resources reach the legacy fence allocator independently
    # of engine start. Its constructor and destructor each take physical
    # force-wake and write the 0x100000 fence-register bank through MMIO+0x1240.
    # A null allocator result is a native fail-closed boundary: both resource
    # and display callers check it and unwind without creating an IGFence whose
    # later free method would repeat the physical writes.
    if len(direct_branches(FENCE_ALLOCATE, FENCE_INIT)) != 1:
        raise AssertionError(f"{path}: fence allocator/init edge changed")
    if len(direct_branches(RESOURCE_ADD_APERTURE, FENCE_ALLOCATE)) != 1 or \
            len(direct_branches(DISPLAY_ALLOC_SCANOUT, FENCE_ALLOCATE)) != 1:
        raise AssertionError(f"{path}: legacy fence allocation callers changed")
    for owner, anchor in ((FENCE_INIT, FENCE_INIT_MMIO_ANCHOR),
                          (FENCE_FREE, FENCE_FREE_MMIO_ANCHOR)):
        body = image[value(owner):next_symbol(value(owner))]
        if body.count(anchor) != 1 or \
                len(direct_branches(owner, SAFE_FORCE_WAKE)) != 2:
            raise AssertionError(
                f"{path}: physical fence-register protocol changed in {owner}")
    resource_start = value(RESOURCE_ADD_APERTURE)
    resource_body = image[resource_start:next_symbol(resource_start)]
    if resource_body.count(FENCE_RESOURCE_NULL_UNWIND) != 1:
        raise AssertionError(
            f"{path}: aperture resource no longer unwinds a null fence")

    # IntelAccelerator's factory slot constructs IntelTGLMemoryManager. Its
    # base init zeroes the two eDRAM capability bytes and then unconditionally
    # dispatches virtual slot 0x148 to the TGL detector. That detector accesses
    # physical eDRAM capability/control registers before engine start, including
    # a property-dependent programming branch. Force-wake suppression alone is
    # not containment because every raw load/store remains reachable.
    accelerator_vtable = value(ACCELERATOR_VTABLE)
    if struct.unpack_from(
            "<Q", image, accelerator_vtable + 16 + 0xAF0)[0] != value(
                NEW_MEMORY_MANAGER):
        raise AssertionError(
            f"{path}: accelerator memory-manager factory slot changed")
    memory_vtable = value(TGL_MEMORY_VTABLE)
    if struct.unpack_from(
            "<Q", image, memory_vtable + 16 + 0x148)[0] != value(
                TGL_DETECT_EDRAM):
        raise AssertionError(f"{path}: TGL eDRAM virtual slot changed")
    factory_start = value(NEW_MEMORY_MANAGER)
    factory_end = next_symbol(factory_start)
    metaclass_refs = []
    for candidate in range(factory_start, factory_end - 6):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 05"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == value(TGL_MEMORY_METACLASS):
            metaclass_refs.append(candidate)
    if len(metaclass_refs) != 1:
        raise AssertionError(
            f"{path}: memory-manager factory no longer selects TGL metaclass")
    memory_init_start = value(MEMORY_MANAGER_INIT)
    memory_init_body = image[memory_init_start:next_symbol(memory_init_start)]
    if memory_init_body.count(MEMORY_EDRAM_ZERO_STATE) != 1 or \
            memory_init_body.count(bytes.fromhex("ff 90 48 01 00 00")) != 1:
        raise AssertionError(
            f"{path}: base memory-manager eDRAM init/dispatch changed")
    edram_start = value(TGL_DETECT_EDRAM)
    edram_body = image[edram_start:next_symbol(edram_start)]
    for anchor in (EDRAM_CAPABILITY_MMIO_READ, EDRAM_CONTROL_MMIO_WRITES,
                   EDRAM_CONFIRM_MMIO_READ):
        if edram_body.count(anchor) != 1:
            raise AssertionError(
                f"{path}: TGL physical eDRAM MMIO inventory changed")
    if len(direct_branches(TGL_DETECT_EDRAM, SAFE_FORCE_WAKE)) != 4:
        raise AssertionError(
            f"{path}: TGL eDRAM force-wake inventory changed")
    dpsm_refs = []
    for candidate in range(accelerator_start, accelerator_start_end - 6):
        if image[candidate:candidate + 3] != bytes.fromhex("48 8d 35"):
            continue
        displacement = struct.unpack_from("<i", image, candidate + 3)[0]
        if candidate + 7 + displacement == dpsm_start:
            dpsm_refs.append(candidate)
    if len(dpsm_refs) != 1 or not engine_start_calls[0] < dpsm_refs[0]:
        raise AssertionError(f"{path}: DPSM timer is not the pinned post-engine callback")

    retained_edges = (
        (SCHEDULER4_INIT, SCHEDULER_BASE_INIT),
        (SCHEDULER4_INIT, COMMAND_STREAMER_FACTORY),
        (COMMAND_STREAMER_FACTORY, COMMAND_STREAMER_INIT),
        (COMMAND_STREAMER_INIT, REQUEST_ENABLE_CALLBACK),
        (COMMAND_STREAMER_REGISTER, BRIDGE_REGISTER_TYPE),
        (SCHEDULER4_LOAD_FIRMWARE, GUC_WITH_OPTIONS),
        (GUC_WITH_OPTIONS, GUC_INIT_WITH_OPTIONS),
        (GUC_INIT_WITH_OPTIONS, GUC_INIT_WORK_HISTORY),
        (GUC_INIT_WITH_OPTIONS, GUC_INIT_DOORBELLS),
        (GUC_INIT_WITH_OPTIONS, CTB_WITH_OPTIONS),
        (GUC_INIT_WITH_OPTIONS, GUC_INIT_INTERRUPTS),
        (GUC_INIT_WITH_OPTIONS, GUC_LOAD_BINARY),
        (GUC_INIT_WITH_OPTIONS, GUC_REGISTER_CTB),
        (GUC_INIT_WITH_OPTIONS, CREATE_UK_CONTEXT),
        (GUC_INIT_SCHED_CONTROL, GUC_SETUP_CONTEXT_POOL),
        (GUC_INIT_SCHED_CONTROL, GUC_SETUP_LOG_BUFFERS),
        (GUC_INIT_SCHED_CONTROL, GUC_SETUP_ADDITIONAL),
        (GUC_SETUP_CONTEXT_POOL, MAPPED_WITH_OPTIONS),
        (GUC_SETUP_CONTEXT_POOL, TRANSFER_OWNERSHIP),
        (GUC_SETUP_LOG_BUFFERS, MAPPED_WITH_OPTIONS),
        (GUC_SETUP_LOG_BUFFERS, TRANSFER_OWNERSHIP),
        (GUC_SETUP_ADDITIONAL, MAPPED_WITH_OPTIONS),
        (GUC_SETUP_ADDITIONAL, TRANSFER_OWNERSHIP),
        (GUC_INIT_WORK_HISTORY, MAPPED_WITH_OPTIONS),
        (GUC_INIT_DOORBELLS, GUC_READ_DOORBELLS),
        (MAPPED_WITH_OPTIONS, MAPPED_INIT),
        (CTB_WITH_OPTIONS, CTB_INIT),
        (CTB_INIT, MAPPED_WITH_OPTIONS),
        (CTB_INIT, TRANSFER_OWNERSHIP),
        (GUC_INIT_INTERRUPTS, REQUEST_ENABLE_CALLBACK),
        (GUC_REGISTER_INTERRUPTS, BRIDGE_REGISTER_TYPE),
        (GUC_REGISTER_CTB, GUC_MMIO_ACTION),
        (GUC_REGISTER_CTB, GUC_DEREGISTER_CTB),
        (GUC_DEREGISTER_CTB, GUC_MMIO_ACTION),
        (GUC_FREE, GUC_DEREGISTER_CTB),
        (GUC_FREE, TRANSFER_OWNERSHIP),
        (CTB_FREE, TRANSFER_OWNERSHIP),
    )
    for owner, target in retained_edges:
        if not direct_branches(owner, target):
            raise AssertionError(
                f"{path}: retained native bootstrap edge {owner} -> {target} changed")

    # The VF wrapper deliberately preserves native stop so Tahoe can finish its
    # software event lifecycle and release every scheduler-owned object.  Its
    # only engine boundary must remain the routed stopGraphicsEngine call, and
    # all trace/sysctl teardown must follow that routed engine-stop boundary.
    # Call order alone does not establish successful GPU DMA quiescence.
    finish_calls = direct_branches(ACCELERATOR_STOP, EVENT_FINISH_ALL)
    trace_disable_calls = direct_branches(ACCELERATOR_STOP, TRACE_DISABLE)
    engine_stop_calls = direct_branches(ACCELERATOR_STOP, STOP)
    trace_shutdown_calls = direct_branches(ACCELERATOR_STOP, TRACE_SHUTDOWN)
    unregister_calls = direct_branches(ACCELERATOR_STOP, UNREGISTER_SYSCTL)
    if (len(finish_calls) != 1 or len(trace_disable_calls) != 1 or
            len(engine_stop_calls) != 1 or len(trace_shutdown_calls) != 2 or
            len(unregister_calls) != 1):
        raise AssertionError(f"{path}: native accelerator-stop call inventory changed")
    engine_stop = engine_stop_calls[0]
    if not (finish_calls[0] < trace_disable_calls[0] < engine_stop and
            all(engine_stop < call for call in trace_shutdown_calls) and
            engine_stop < unregister_calls[0]):
        raise AssertionError(
            f"{path}: native accelerator stop no longer calls engine stop before teardown")
    accelerator_stop_end = next_symbol(value(ACCELERATOR_STOP))
    release_calls = []
    for pattern in (bytes.fromhex("ff 50 28"),
                    bytes.fromhex("ff 90 28 00 00 00")):
        cursor = value(ACCELERATOR_STOP)
        while True:
            cursor = image.find(pattern, cursor, accelerator_stop_end)
            if cursor < 0:
                break
            release_calls.append(cursor)
            cursor += 1
    if not release_calls or any(call < engine_stop for call in release_calls):
        raise AssertionError(
            f"{path}: native accelerator stop releases an object before routed engine stop")

    # The runtime patch must search across the private global constructor.
    # Its production bounds deliberately use exported symbols because the
    # binary contains many identically named __GLOBAL__D_a local symbols.
    blit3d_start = value(BLIT3D_BOUNDS_START)
    blit3d_end = value(BLIT3D_BOUNDS_END)
    blit3d_global_init = value(BLIT3D_GLOBAL_INIT)
    blit3d_anchors = []
    cursor = 0
    while True:
        cursor = image.find(BLIT3D_SCRATCH_ANCHOR, cursor)
        if cursor < 0:
            break
        blit3d_anchors.append(cursor)
        cursor += 1
    if len(blit3d_anchors) != 1:
        raise AssertionError(
            f"{path}: expected one Blit3D scratch constructor anchor")
    blit3d_anchor = blit3d_anchors[0]
    if not (blit3d_start < blit3d_global_init < blit3d_anchor < blit3d_end):
        raise AssertionError(
            f"{path}: exported Blit3D patch bounds do not enclose constructor anchor")
    if blit3d_end - blit3d_start > 0x400:
        raise AssertionError(f"{path}: Blit3D patch bounds exceed runtime limit")

    for lifecycle, owner, bridge in (
            (ENABLE, START, BRIDGE_ENABLE),
            (DISABLE, STOP, BRIDGE_DISABLE)):
        relocation = relocations.get(lifecycle)
        owner_start = value(owner)
        if relocation is None or not owner_start < relocation < next_symbol(owner_start):
            raise AssertionError(
                f"{path}: {lifecycle} is not called by {owner}")
        opcode = relocation - 1
        if image[opcode] != 0xE8:
            raise AssertionError(f"{path}: {lifecycle} is not a direct call")

        bridge_target = value(bridge)
        preceding = []
        for candidate in range(max(owner_start, opcode - 32), opcode):
            if image[candidate] != 0xE8 or candidate + 5 > len(image):
                continue
            displacement = struct.unpack_from("<i", image, candidate + 1)[0]
            if candidate + 5 + displacement == bridge_target:
                preceding.append(candidate)
        if not preceding:
            raise AssertionError(
                f"{path}: {bridge} does not precede {lifecycle}")

    for scheduler, scheduler_error, streamer in (
            (SCHEDULER_ENABLE, SCHEDULER_ERROR_ENABLE, STREAMER_ERROR_ENABLE),
            (SCHEDULER_DISABLE, SCHEDULER_ERROR_DISABLE, STREAMER_ERROR_DISABLE)):
        if not direct_branches(scheduler, scheduler_error):
            raise AssertionError(
                f"{path}: {scheduler} no longer dispatches {scheduler_error}")
        if not direct_branches(scheduler_error, streamer):
            raise AssertionError(
                f"{path}: {scheduler_error} no longer owns the physical {streamer} call")

    # TGL/ADL/RPL VFs use this native Gen11 virtual-interrupt register block.
    # Treat every aligned 0x190000-range displacement in the live bridge bodies
    # as MMIO and prove that it remains inside i915's VF allowlist.
    vf_ranges = (
        (0x190010, 0x190010), (0x190018, 0x19001C),
        (0x190030, 0x190048), (0x190060, 0x190064),
        (0x190070, 0x190074), (0x190090, 0x190090),
        (0x1900A0, 0x1900A0), (0x1900A8, 0x1900AC),
        (0x1900B0, 0x1900B4), (0x1900D0, 0x1900D4),
        (0x1900E8, 0x1900EC), (0x1900F0, 0x1900F4),
        (0x190100, 0x190100),
    )

    def mmio_offsets(name):
        start = value(name)
        body = image[start:next_symbol(start)]
        offsets = set()
        for cursor in range(len(body) - 3):
            candidate = struct.unpack_from("<I", body, cursor)[0]
            if 0x190000 <= candidate <= 0x190400 and candidate % 4 == 0:
                offsets.add(candidate)
        return offsets

    reader_helpers = (
        BRIDGE_READ_RCS, BRIDGE_READ_CCS, BRIDGE_READ_BCS,
        BRIDGE_READ_GUC, BRIDGE_READ_VCS, BRIDGE_READ_VECS,
    )
    bridge_offsets = {}
    for name in (BRIDGE_ENABLE, BRIDGE_DISABLE, BRIDGE_FILTER, BRIDGE_READ,
                 BRIDGE_ENABLE_INTERRUPTS, BRIDGE_DISABLE_INTERRUPTS,
                 *reader_helpers):
        offsets = mmio_offsets(name)
        bridge_offsets[name] = offsets
        for offset in offsets:
            if not any(first <= offset <= last for first, last in vf_ranges):
                raise AssertionError(
                    f"{path}: native VF interrupt body uses non-allowlisted MMIO {offset:#x}")
    if bridge_offsets[BRIDGE_ENABLE] != {0x190010}:
        raise AssertionError(f"{path}: bridge enable master-register contract changed")
    if bridge_offsets[BRIDGE_DISABLE] != {0x190010}:
        raise AssertionError(f"{path}: bridge disable master-register contract changed")
    if bridge_offsets[BRIDGE_FILTER] != {0x190010}:
        raise AssertionError(f"{path}: bridge filter master-register contract changed")
    required_reader = {0x190010, 0x190018, 0x19001C}
    if bridge_offsets[BRIDGE_READ] != required_reader:
        raise AssertionError(f"{path}: native virtual-MMIO reader inventory changed")
    for helper in reader_helpers:
        if not direct_branches(BRIDGE_READ, helper):
            raise AssertionError(f"{path}: native reader no longer calls {helper}")
    for helper in reader_helpers[:4]:
        if bridge_offsets[helper] != {0x190060, 0x190070}:
            raise AssertionError(f"{path}: bank-0 reader inventory changed for {helper}")
    for helper in reader_helpers[4:]:
        if bridge_offsets[helper] != {0x190064, 0x190074}:
            raise AssertionError(f"{path}: bank-1 reader inventory changed for {helper}")
    required_enable = {
        0x190030, 0x190034, 0x190038, 0x19003C, 0x190040, 0x190044,
        0x190048, 0x190060, 0x190064, 0x190070, 0x190074,
    }
    if not required_enable <= bridge_offsets[BRIDGE_ENABLE_INTERRUPTS]:
        raise AssertionError(f"{path}: native virtual-MMIO enable inventory changed")
    required_disable = {0x190030, 0x190034, 0x190038, 0x19003C, 0x190040, 0x190044}
    if not required_disable <= bridge_offsets[BRIDGE_DISABLE_INTERRUPTS]:
        raise AssertionError(f"{path}: native virtual-MMIO disable inventory changed")

    # enable() tests +0x8a8 at entry and jumps past its callback-list walk when
    # it is already true. requestEnableCallback() merely appends to +0x8b8 and
    # does not compensate for that one-shot boundary. GuC init registers only
    # after creating its software event source, proving the VF wrapper must run
    # a post-enable registration immediately.
    enable_start = value(BRIDGE_ENABLE)
    enable_body = image[enable_start:next_symbol(enable_start)]
    if not enable_body.startswith(b"\x55\x48\x89\xe5\x41\x56\x53\x80\xbf\xa8\x08\x00\x00\x00"):
        raise AssertionError(f"{path}: bridge enabled-state entry guard changed")
    request_start = value(REQUEST_ENABLE_CALLBACK)
    request_body = image[request_start:next_symbol(request_start)]
    if b"\x49\x89\x9f\xb8\x08\x00\x00" not in request_body:
        raise AssertionError(f"{path}: callback request no longer appends to +0x8b8")
    if not direct_branches(GUC_INIT_INTERRUPTS, REQUEST_ENABLE_CALLBACK):
        raise AssertionError(f"{path}: GuC init no longer registers through the bridge request")

    # createUkContext gets the private IGAccelMemory owner and invokes its
    # two-argument virtual getPhysicalSegment slot. The concrete system-memory
    # method supplies MemoryManager+0x88 mapper options to the owned IOMD. This
    # is not IOMemoryDescriptor's public three-argument virtual ABI.
    if not direct_branches(CREATE_UK_CONTEXT, MAPPED_GET_MEMORY):
        raise AssertionError(f"{path}: createUkContext no longer obtains IGAccelMemory")
    create_start = value(CREATE_UK_CONTEXT)
    create_body = image[create_start:next_symbol(create_start)]
    if create_body.count(b"\xff\x91\x58\x01\x00\x00") != 1:
        raise AssertionError(
            f"{path}: createUkContext physical-segment virtual ABI changed")
    physical_start = value(SYS_MEMORY_PHYSICAL)
    physical_body = image[physical_start:next_symbol(physical_start)]
    for sequence in (
            b"\x48\x8b\x47\x28",              # accelerator at +0x28
            b"\x48\x8b\xbf\xd0\x00\x00\x00",  # owned IOMD at +0xd0
            b"\x8b\x88\x88\x00\x00\x00",      # mapper options at +0x88
            b"\xff\x90\x38\x01\x00\x00"):       # IOMD segment method +0x138
        if sequence not in physical_body:
            raise AssertionError(
                f"{path}: IGAccelSysMemory mapper-aware segment ABI changed")

    print(f"PASS: native bridge/IOAccel lifecycle order in {path}")


def function_body(source, signature):
    cursor = 0
    while True:
        start = source.index(signature, cursor)
        opening = source.find("{", start + len(signature))
        declaration = source.find(";", start + len(signature))
        if opening >= 0 and (declaration < 0 or opening < declaration):
            break
        cursor = start + len(signature)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated {signature}")


def ring_backing_submit_contract(source, path="<source>"):
    submit = function_body(source, "bool Gen11::vfSubmitWorkItem(")
    for requirement in (
            "OSObject *admittedRingBacking = admitted ? gVfContexts[admittedSlot].ringBacking : nullptr;",
            "!admittedRingBacking || ringBacking != admittedRingBacking ||",
            "nativeRingSize != ringSize || (nativeRingSize & (nativeRingSize - 1U)) != 0 ||",
            "nativeRingMask != nativeRingSize - 1U",
            "getMember<uint64_t>(admittedRingBacking, kVfMappedBufferLengthOffset) < ringSize"):
        assert requirement in submit, f"{path}: missing retained ring submission guard"
    ring_fault = submit.index('vfMarkProtocolFault("submit ring backing identity or extent mismatch")')
    assert "return false;" in submit[ring_fault:submit.index("}", ring_fault)], f"{path}: ring mismatch branch must reject locally"
    geometry_fault = submit.index('vfMarkProtocolFault("submit ring control and native geometry mismatch")')
    assert "return false;" in submit[geometry_fault:submit.index("}", geometry_fault)], f"{path}: geometry mismatch must reject locally"
    assert geometry_fault < submit.index("const uint32_t previousRingTail"), f"{path}: geometry must be validated before publication"
    assert ring_fault < submit.index("return false;", ring_fault) < submit.index("const uint32_t previousRingTail"), f"{path}: ring mismatch must reject before tail publication"


def ring_backing_submit_mutations(path):
    source = pathlib.Path(path).read_text()
    mutations = (
        ("nativeRingMask != nativeRingSize - 1U", "false"),
        ("nativeRingSize != ringSize || (nativeRingSize & (nativeRingSize - 1U)) != 0 ||", "false ||"),
        ("!admittedRingBacking || ringBacking != admittedRingBacking ||",
         "!admittedRingBacking || false ||"),
        ("getMember<uint64_t>(admittedRingBacking, kVfMappedBufferLengthOffset) < ringSize",
         "getMember<uint64_t>(admittedRingBacking, kVfMappedBufferLengthOffset) > ringSize"),
        ('vfMarkProtocolFault("submit ring backing identity or extent mismatch");\n\t\treturn false;',
         'vfMarkProtocolFault("submit ring backing identity or extent mismatch");\n\t\t/* continue incorrectly */'),
    )
    for old, new in mutations:
        assert source.count(old) == 1, "ring guard mutation must have one exact target"
        try:
            ring_backing_submit_contract(source.replace(old, new, 1))
        except (AssertionError, ValueError):
            continue
        raise AssertionError("ring backing guard mutation escaped source contract")
    print("PASS: five ring backing/geometry guard mutations rejected (source contract, not DMA proof)")


def g2h_event_transaction_contract(source, path):
    body = function_body(source, "bool Gen11::vfCtbGucToHostAction(void *that, uint32_t *message)")
    for token in ("explicit ConsumerTransaction(IOLock *value) : lock(value) { IOLockLock(lock); }",
                  "~ConsumerTransaction() { IOLockUnlock(lock); }",
                  "} consumerTransaction(lock);",
                  "ConsumerTransaction(const ConsumerTransaction &) = delete;"):
        if token not in body:
            raise AssertionError(f"{path}: G2H event transaction lacks {token}")
    if body.count("IOLockUnlock(lock);") != 1 or body.count("IOLockLock(lock);") != 1:
        raise AssertionError(f"{path}: G2H transaction has an unscoped lock operation")
    admission = body.index("} consumerTransaction(lock);")
    read = body.index("NGGuCRing::readFrame(")
    if "if (!vfCtbConsumerReady(false))" not in body[admission:read]:
        raise AssertionError(f"{path}: queued G2H consumer lacks locked readiness recheck")
    for forbidden in ("IOSleep(", "pollVfGuCToHost(", "vfSendCtbFastAction(",
                      "vfInvalidateTLBSync(", "FunctionCast("):
        if forbidden in body:
            raise AssertionError(f"{path}: G2H transaction gained a blocking/reentrant dependency")


def g2h_event_transaction_mutations(path):
    source = pathlib.Path(path).read_text()
    mutations = (
        ("} consumerTransaction(lock);", "};"),
        ("\tdescriptor[4] = head;\n", "\tdescriptor[4] = head;\n\tIOLockUnlock(lock);\n"),
        ("\tif (!vfCtbConsumerReady(false))\n\t\treturn false;\n",
         "\tif (false)\n\t\treturn false;\n"),
    )
    for before, after in mutations:
        # The descriptor-head store also occurs elsewhere; bound mutations to
        # this consumer body so an unrelated writer cannot satisfy the test.
        body = function_body(source, "bool Gen11::vfCtbGucToHostAction(void *that, uint32_t *message)")
        assert body.count(before) == 1, f"ambiguous G2H mutation: {before}"
        changed = source.replace(body, body.replace(before, after, 1), 1)
        try:
            g2h_event_transaction_contract(changed, path)
        except AssertionError:
            continue
        raise AssertionError(f"{path}: escaped G2H transaction mutation")
    print("PASS: three G2H event transaction mutations rejected (source contract, not concurrency proof)")


def g2h_completion_lock_contract(source, path):
    """Selected direct-poll graph only; not a whole-driver deadlock proof.

    TLB waits hold gVfGucLock while polling. Completion cannot reacquire that
    lock, the H2G producer lock, or an outer accelerator mutex. Check helpers
    too: inspecting just the event transaction would miss a nested dependency.
    Native dispatcher body/route identity is checked separately.
    """
    signatures = (
        "bool Gen11::pollVfGuCToHost(void *that)",
        "bool Gen11::vfDrainGuCToHost(void *that, IOInterruptEventSource *source,",
        "bool Gen11::vfCtbGucToHostAction(void *that, uint32_t *message)",
        "static bool vfCtbConsumerReady(bool requireInterrupt)",
        "static bool vfInterruptTransportReady()",
        "bool vfCanUseSleepingLock()",
        "bool vfValidG2HMapping(const volatile uint32_t *descriptor,",
        "bool vfEnterIrqCallback()",
        "void vfLeaveIrqCallback()",
        "bool vfG2HCtbPending(uint32_t &head)",
        "void vfReleaseG2HCredits(uint32_t count)",
        "void vfMarkProtocolFault(const char *reason)",
    )
    for signature in signatures:
        body = function_body(source, signature)
        for forbidden in ("gVfGucLock", "vfSendCtbFastAction(",
                          "vfInvalidateTLBSync(", "FunctionCast(",
                          "0x88)", "0x18)"):
            if forbidden in body:
                raise AssertionError(f"{path}: completion graph acquired producer/outer dependency in {signature}: {forbidden}")
        # The consumer's sole sleeping mutex is the explicitly scoped G2H
        # transaction. Its context spin-lock operations are separately pinned.
        expected_locks = 1 if "vfCtbGucToHostAction" in signature else 0
        if body.count("IOLockLock(") != expected_locks:
            raise AssertionError(f"{path}: completion graph gained a sleeping lock in {signature}")
    wait = function_body(source, "bool vfInvalidateTLBSync(void *guc)")
    assert wait.index("IOLockLock(gVfGucLock);") < wait.index("pollVfGuCToHost(guc)") < wait.index("IOLockUnlock(gVfGucLock);"), f"{path}: changed TLB waiter lock/poll boundary"


def g2h_completion_lock_mutations(path):
    source = pathlib.Path(path).read_text()
    for signature, dependency in (
            ("void vfReleaseG2HCredits(uint32_t count)", "IOLockLock(gVfGucLock);"),
            ("bool Gen11::vfDrainGuCToHost(void *that, IOInterruptEventSource *source,", "IOLockLock(getMember<IOLock *>(that, 0x88));"),
            ("bool vfG2HCtbPending(uint32_t &head)", "IOLockLock(getMember<IOLock *>(that, 0x18));"),
            ("void vfMarkProtocolFault(const char *reason)", "vfInvalidateTLBSync(nullptr);")):
        body = function_body(source, signature)
        changed = source.replace(body, "{\n" + dependency + body[1:], 1)
        try:
            g2h_completion_lock_contract(changed, path)
        except AssertionError:
            continue
        raise AssertionError(f"{path}: completion lock dependency mutation escaped")
    print("PASS: four selected completion lock-dependency mutations rejected (source graph, not runtime deadlock proof)")


def source_contract(path):
    source = pathlib.Path(path).read_text()
    ring_backing_submit_contract(source, path)
    for signature in ("bool Gen11::vfAttachContextDesc(",
                      "void Gen11::vfDetachContextDesc(",
                      "bool Gen11::vfSubmitWorkItem("):
        assert "VfContextOperationGuard operationGuard;" in function_body(source, signature), f"{path}: lost counted context-operation admission: {signature}"
    required = (
        "com.apple.iokit.IOAcceleratorFamily2",
        "com.apple.iokit.IOPCIFamily",
        ENABLE,
        DISABLE,
        EVENT_INIT,
        "this->vfInterruptBridgeEnable = irqEnable",
        "this->vfInterruptBridgeDisable = irqDisable",
        SCHEDULER_ENABLE,
        SCHEDULER_DISABLE,
        PCI_CONFIGURE_INTERRUPTS,
        SCHEDULER_INIT_FIRMWARE,
        REQUEST_ENABLE_CALLBACK,
        SYS_MEMORY_PHYSICAL,
        BRIDGE_FILTER,
        BRIDGE_READ,
        BRIDGE_ENABLE_INTERRUPTS,
        BRIDGE_DISABLE_INTERRUPTS,
        SCHEDULER_ERROR_ENABLE,
        SCHEDULER_ERROR_DISABLE,
        SCHEDULER_HALT,
        SCHEDULER_RESUME,
        ENCODE_DEBUG,
        RING_DO_HANG,
        RING_DUMP_HANG,
        FIFO_RESET_REPLAY,
        RING_RESET_GRAPHICS,
    )
    for token in required:
        if token not in source:
            raise AssertionError(f"{path}: missing lifecycle token {token}")

    for token in (
            "gVfUsesMemoryIrq = vfActive &&",
            "NGGpuCapabilities::hasIovMemoryIrq(gpuDevice)",
            "gVfMemIrqConfigured && gVfMemIrqRequested != 0",
            "gVfMmioIrqReady != 0"):
        if token not in source:
            raise AssertionError(f"{path}: missing interrupt-transport capability gate {token}")

    pci_resolution = function_body(
        source,
        "bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)")
    if "index == kextIOPCIFamily.loadIndex" not in pci_resolution:
        raise AssertionError(f"{path}: PCI allocator is not resolved from IOPCIFamily")
    if ("KernelPatcher::KernelID" in pci_resolution and
            pci_resolution.index("KernelPatcher::KernelID") <
            pci_resolution.index(PCI_CONFIGURE_INTERRUPTS)):
        raise AssertionError(f"{path}: IOPCIFamily symbol is incorrectly resolved from KernelID")
    normalized_pci_resolution = "".join(pci_resolution.split())
    if ('{"' + START + '",startGraphicsEngine},' not in
            normalized_pci_resolution):
        raise AssertionError(
            f"{path}: VF no longer replaces the complete physical engine-start body")

    timeout_routes_start = pci_resolution.rfind(
        "KernelPatcher::RouteRequest requests[]", 0,
        pci_resolution.index(FENCE_ALLOCATE))
    if timeout_routes_start < 0:
        raise AssertionError(f"{path}: missing VF accelerator route table")
    timeout_routes_end = pci_resolution.index(
        "Failed to route VF accelerator symbols", timeout_routes_start)
    timeout_routes = "".join(
        pci_resolution[timeout_routes_start:timeout_routes_end].split())
    if pci_resolution.rfind("if (vfActive)", 0, timeout_routes_start) < 0:
        raise AssertionError(f"{path}: timeout isolation routes are not VF-only")
    for symbol, wrapper in (
            (EVENT_TIMEOUT, "vfRejectEventTimeout"),
            (SCHEDULER_HALT, "vfSuppressTimeoutHardwareAction"),
            (SCHEDULER_RESUME, "vfSuppressTimeoutHardwareAction"),
            (ENCODE_DEBUG, "vfSuppressPhysicalDebugCapture"),
            (RING_DO_HANG, "vfSuppressHangAnalysis"),
            (RING_DUMP_HANG, "vfSuppressHangDump"),
            (FIFO_RESET_REPLAY, "vfRejectHardwareResetReplay"),
            (RING_RESET_GRAPHICS, "vfRejectPhysicalEngineReset")):
        if '{"' + symbol + '",' + wrapper + '},' not in timeout_routes:
            raise AssertionError(
                f"{path}: missing VF timeout route {symbol} -> {wrapper}")

    timeout_noop = function_body(
        source, "void Gen11::vfSuppressTimeoutHardwareAction(")
    timeout_reject = function_body(source, "void *Gen11::vfRejectEventTimeout(")
    fault = timeout_reject.find("vfMarkProtocolFault(")
    fail_stop = timeout_reject.find("PANIC_COND(true")
    if not (0 <= fault < fail_stop < timeout_reject.find("return nullptr;")):
        raise AssertionError(f"{path}: VF event timeout returns before admission closure/fail-stop")
    for forbidden in ("FunctionCast", "callback->", "getMember", "OSSynchronizeIO",
                      "vfSend", "vfRelease", "OSObject", "0x1240"):
        if forbidden in timeout_reject:
            raise AssertionError(f"{path}: VF timeout fail-stop gained unsafe side effects: {forbidden}")
    debug_noop = function_body(
        source, "void Gen11::vfSuppressPhysicalDebugCapture(")
    hang_analysis = function_body(
        source, "uint32_t Gen11::vfSuppressHangAnalysis(")
    hang_dump = function_body(source, "void Gen11::vfSuppressHangDump(")
    for body, arguments, label in (
            (timeout_noop, ("(void)that;", "(void)engine;"), "timeout action"),
            (debug_noop, ("(void)that;", "(void)reason;"), "debug capture"),
            (hang_analysis, ("(void)that;", "return 0;"), "hang analysis"),
            (hang_dump, ("(void)that;",), "hang dump")):
        if any(token not in body for token in arguments):
            raise AssertionError(f"{path}: incomplete VF {label} replacement")
        for forbidden in ("FunctionCast", "callback->", "getMember", "0x1240",
                          "SafeForceWake", "MMIO"):
            if forbidden in body:
                raise AssertionError(
                    f"{path}: VF {label} replacement re-enters hardware through {forbidden}")

    reset_replay = function_body(
        source, "void Gen11::vfRejectHardwareResetReplay(")
    engine_reset = function_body(
        source, "bool Gen11::vfRejectPhysicalEngineReset(")
    if "vfMarkProtocolFault(" not in reset_replay or \
            "physical engine reset/replay requested on VF" not in reset_replay:
        raise AssertionError(f"{path}: VF reset/replay root does not quarantine transport")
    for token in ("(void)that;", "(void)context;", "vfMarkProtocolFault(",
                  "physical engine reset requested on VF", "return false;"):
        if token not in engine_reset:
            raise AssertionError(
                f"{path}: VF physical engine-reset rejection is incomplete: {token}")
    for body, label in ((reset_replay, "reset/replay"),
                        (engine_reset, "physical engine reset")):
        for forbidden in ("FunctionCast", "callback->", "getMember", "0x1240",
                          "SafeForceWake", "MMIO"):
            if forbidden in body:
                raise AssertionError(
                    f"{path}: VF {label} rejection re-enters hardware through {forbidden}")

    for token in (
            GUC_INIT_SCHED_CONTROL,
            GUC_LOAD_BINARY,
            GUC_INIT_DOORBELLS,
            CTB_INIT,
            GUC_READ_DOORBELLS,
            REQUEST_ENABLE_CALLBACK,
            CREATE_UK_CONTEXT,
            GUC_MMIO_ACTION,
            MAPPED_WITH_OPTIONS,
            TRANSFER_OWNERSHIP,
            GLOBAL_MAP_RANGE,
            GLOBAL_MAP_ROTATED,
            GLOBAL_UNMAP_RANGE,
            GLOBAL_MAP_DUMMY):
        if token not in pci_resolution:
            raise AssertionError(
                f"{path}: retained native bootstrap descendant is not VF-routed: {token}")
    if FENCE_ALLOCATE not in pci_resolution:
        raise AssertionError(f"{path}: physical fence allocator is not VF-routed")
    if ('{"' + FENCE_ALLOCATE + '",vfRejectPhysicalFence},' not in
            normalized_pci_resolution):
        raise AssertionError(f"{path}: physical fence route mapping changed")
    fence_reject = function_body(
        source, "void *Gen11::vfRejectPhysicalFence(void *that,")
    if "return nullptr;" not in fence_reject:
        raise AssertionError(f"{path}: VF fence rejection no longer returns null")
    for forbidden in ("FunctionCast", "callback->", "0x1240", "SafeForceWake"):
        if forbidden in fence_reject:
            raise AssertionError(
                f"{path}: VF fence rejection re-enters hardware through {forbidden}")
    if ('{"' + TGL_DETECT_EDRAM + '",vfDisableEdramProbe},' not in
            normalized_pci_resolution):
        raise AssertionError(f"{path}: physical eDRAM detector is not VF-routed")
    edram_disable = function_body(
        source, "void Gen11::vfDisableEdramProbe(void *that)")
    for token in (
            "getMember<uint8_t>(that, 0x20) = 0",
            "getMember<uint8_t>(that, 0x21) = 0"):
        if token not in edram_disable:
            raise AssertionError(
                f"{path}: VF eDRAM capability state is not cleared: {token}")
    for forbidden in ("FunctionCast", "callback->", "SafeForceWake", "MMIO"):
        if forbidden in edram_disable:
            raise AssertionError(
                f"{path}: VF eDRAM route re-enters hardware through {forbidden}")

    load_guc = function_body(source, "bool Gen11::loadGuCBinary(void *that)")
    for token in (
            "callback->orgInitSchedControl",
            "getMember<void *>(that, 0x50)",
            "getMember<void *>(that, 0x68)",
            "getMember<void *>(that, 0x60)",
            "getMember<void *>(that, 0x70)",
            "getMember<void *>(that, 0x78)",
            "getMember<void *>(that, 0x9E8)"):
        if token not in load_guc:
            raise AssertionError(
                f"{path}: native VF scheduler storage contract is incomplete: {token}")

    memory_routes_start = pci_resolution.index(
        "KernelPatcher::RouteRequest memoryIrqRoutes[]")
    virtual_routes_start = pci_resolution.index(
        "KernelPatcher::RouteRequest virtualMmioIrqRoutes[]", memory_routes_start)
    virtual_routes_end = pci_resolution.index(
        '"Failed to isolate VF physical engine error interrupts"', virtual_routes_start)
    memory_routes = pci_resolution[memory_routes_start:virtual_routes_start]
    virtual_routes = pci_resolution[virtual_routes_start:virtual_routes_end]
    for token in (BRIDGE_FILTER, BRIDGE_READ, BRIDGE_ENABLE_INTERRUPTS,
                  BRIDGE_DISABLE_INTERRUPTS, SCHEDULER_ENABLE, SCHEDULER_DISABLE):
        if token not in memory_routes:
            raise AssertionError(f"{path}: memory-IRQ route set is missing {token}")
        if token in virtual_routes:
            raise AssertionError(f"{path}: virtual-MMIO route wrongly replaces {token}")
    for token in (SCHEDULER_ERROR_ENABLE, SCHEDULER_ERROR_DISABLE,
                  "vfSuppressPhysicalErrorInterrupts"):
        if token not in virtual_routes:
            raise AssertionError(f"{path}: virtual-MMIO isolation is missing {token}")

    master_patch = pci_resolution.index("static const uint8_t masterDisable[]")
    memory_gate = pci_resolution.rfind("if (gVfUsesMemoryIrq)", 0, master_patch)
    if memory_gate < 0 or not memory_routes_start > master_patch > memory_gate:
        raise AssertionError(f"{path}: GFX_MSTR_IRQ patch is not memory-IRQ-only")

    configure_memirq = function_body(source, "bool vfConfigureMemIrq()")
    if "!gVfUsesMemoryIrq" not in configure_memirq:
        raise AssertionError(f"{path}: unsupported devices can configure memory IRQ")
    consume_memirq = function_body(source, "uint64_t vfConsumeMemoryInterrupts()")
    if "!gVfUsesMemoryIrq" not in consume_memirq:
        raise AssertionError(f"{path}: unsupported devices can consume memory IRQ")
    configure_ctb = function_body(
        source, "bool vfConfigureModernCtb(bool g2h, uint32_t appleDescriptorAddress)")
    if "g2h && gVfUsesMemoryIrq && !vfConfigureMemIrq()" not in configure_ctb:
        raise AssertionError(f"{path}: CTB registration does not capability-gate memory IRQ")
    attach = function_body(source, "bool Gen11::vfAttachContextDesc(void *that, const uint32_t *descriptor)")
    if "!gVfUsesMemoryIrq ||" not in attach or "vfPrepareContextMemoryIrq(" not in attach:
        raise AssertionError(f"{path}: LRCA memory-IRQ mutation is not capability-gated")
    if not attach.index("NGVfContextShutdown::validPacketBacking(stampIndex, stampBytes, scratchBytes)") < \
            attach.index("contextBacking->retain();") < attach.index("vfSendCtbFastAction(that, request,"):
        raise AssertionError(f"{path}: packet backing bounds are not checked before registration")
    for token in ("!ringBacking ||", "entry.ringBacking != ringBacking",
                  "kVfContextRingObjectOffset", "kVfRingMappedBufferOffset"):
        if token not in attach:
            raise AssertionError(f"{path}: direct context ring ownership lacks {token}")
    if not attach.index("ringBacking->retain();") < attach.index(
            "entry.ringBacking = ringBacking;") < attach.index(
                "entry.state = kVfGucContextRegistering;") < attach.index(
                    "vfSendCtbFastAction(that, request,"):
        raise AssertionError(f"{path}: ring DMA backing is not pinned before registration")
    partial_fault = 'vfMarkProtocolFault("failed to retire partially registered GuC context");'
    if not attach[attach.index(partial_fault) + len(partial_fault):].lstrip().startswith(
            'PANIC_COND(true, "ngreen",'):
        raise AssertionError(f"{path}: partially owned context can return into native initialization")
    retire = function_body(source, "void vfReleaseRetiredContextBacking(uint16_t gucId)")
    for token in ("!gVfProtocolFault", "entry.state == kVfGucContextTombstone",
                  "entry.refCount == 0"):
        if token not in retire:
            raise AssertionError(f"{path}: retired backing release lacks {token}")
    if not retire.index("ringBacking = entry.ringBacking;") < retire.index(
            "NGVfContextEvent::clearReleasedIdentity(entry);") < retire.index(
                "IOSimpleLockUnlockEnableInterrupt(") < retire.index(
                    "ringBacking->release();"):
        raise AssertionError(f"{path}: retired DMA ring release/lock order changed")
    for backing in ("stampBacking", "scratchBacking"):
        for token in ("!" + backing + " ||", "entry." + backing + " != " + backing):
            if token not in attach:
                raise AssertionError(f"{path}: context packet backing identity lacks {token}")
        if not attach.index(backing + "->retain();") < attach.index(
                "entry." + backing + " = " + backing + ";") < attach.index(
                    "entry.state = kVfGucContextRegistering;"):
            raise AssertionError(f"{path}: packet backing is not retained before registration")
        if not retire.index(backing + " = entry." + backing + ";") < retire.index(
                "NGVfContextEvent::clearReleasedIdentity(entry);") < retire.index(
                    "IOSimpleLockUnlockEnableInterrupt(") < retire.index(backing + "->release();"):
            raise AssertionError(f"{path}: packet backing release bypasses ownership boundary")
        for signature in ("int32_t vfFindContextLocked(uint32_t lrcaPage)",
                          "int32_t vfReserveContextLocked(uint32_t lrcaPage)",
                          "bool vfDirectContextTableUnowned()"):
            if backing not in function_body(source, signature):
                raise AssertionError(f"{path}: table reuse/ownership ignores {backing}")
    detach = function_body(source, "void Gen11::vfDetachContextDesc(void *that, const uint32_t *descriptor)")
    for reason in (
            "VF detach without valid context bookkeeping",
            "invalid VF context identity before detach",
            "context retirement without pinned H2G queue",
            "duplicate final context detach",
            "VF detach descriptor/backing identity mismatch",
            "VF detach has no direct GuC context record",
            "GuC context teardown timeout"):
        fault = 'vfMarkProtocolFault("' + reason + '");'
        following = detach[detach.index(fault) + len(fault):].lstrip()
        if not following.startswith('PANIC_COND(true, "ngreen",'):
            raise AssertionError(f"{path}: unsafe void detach can return after {reason}")

    drain = function_body(
        source, "bool Gen11::vfDrainGuCToHost(void *that, IOInterruptEventSource *source,")
    g2h_event_transaction_contract(source, path)
    g2h_completion_lock_contract(source, path)
    if "vfCtbConsumerReady(!synchronousPoll)" not in drain:
        raise AssertionError(
            f"{path}: synchronous CTB teardown cannot outlive hardware IRQ disable")

    scratch_start = pci_resolution.index(
        "mach_vm_address_t blit3dBoundsStart")
    scratch_end = pci_resolution.index(
        'SYSLOG("ngreen", "V250:', scratch_start)
    scratch_contract = pci_resolution[scratch_start:scratch_end]
    normalized_scratch_contract = "".join(scratch_contract.split())
    for token in (
            '{"' + BLIT3D_BOUNDS_START + '",blit3dBoundsStart}',
            '{"' + BLIT3D_BOUNDS_END + '",blit3dBoundsEnd}',
            "blit3dBoundsEnd<=blit3dBoundsStart",
            "blit3dBoundsEnd-blit3dBoundsStart>0x400",
            "patcher,blit3dBoundsStart,blit3dBoundsEnd-blit3dBoundsStart"):
        if token not in normalized_scratch_contract:
            raise AssertionError(
                f"{path}: incomplete production Blit3D patch-bound contract: {token}")
    for forbidden in (
            "__ZN25IGHardwareExtendedContext9MetaClassD0Ev",
            "__ZN23IGHardwareBlit3DContext9MetaClassD0Ev"):
        if forbidden in scratch_contract:
            raise AssertionError(
                f"{path}: Blit3D anchor is bounded by a symbol before its address")

    start = function_body(source, "bool Gen11::startGraphicsEngine(void *that)")
    for forbidden in ("FunctionCast", "0x1240", "SafeForceWake",
                      "initModeRegisters", "initHardwareStatusPageRegisters"):
        if forbidden in start:
            raise AssertionError(
                f"{path}: VF engine-start re-enters physical state through {forbidden}")
    bridge = start.index("callback->vfInterruptBridgeEnable)(")
    firmware = start.index("callback->vfSchedulerInitFirmware)(scheduler)")
    ready = start.index("if (!vfNativeGpuWorkReady())")
    accelerator = start.index("callback->ioGraphicsEnableAccelerator)(that)")
    if not bridge < firmware < ready < accelerator:
        raise AssertionError(
            f"{path}: VF MSI/firmware/transport/accelerator lifecycle order is reversed")
    failure = start.index("VF scheduler firmware initialization failed")
    disable = start.index("callback->vfInterruptBridgeDisable)(", failure)
    fault = start.index("VF scheduler firmware initialization failure", failure)
    if not failure < disable < fault:
        raise AssertionError(
            f"{path}: firmware failure does not close the early VF MSI consumer")
    if "IGMemoryManager::initCache" not in start or "must omit it" not in start:
        raise AssertionError(
            f"{path}: physical VF cache initialization is not explicitly excluded")
    if start.index("vfInterruptBridgeEnable") > accelerator:
        raise AssertionError(f"{path}: VF start lifecycle order is reversed")
    if start.count("initEvent(eventMachine") != 2:
        raise AssertionError(f"{path}: VF start does not initialize both native events")
    stop = function_body(source, "bool Gen11::stopGraphicsEngine(void *that)")
    timer = stop.index("getMember<IOTimerEventSource *>(that, 0x1460)")
    cancel = stop.index("dpsmTimer->cancelTimeout();", timer)
    shutdown = stop.index("vfQuiesceDeviceForShutdown(")
    assert timer < cancel < shutdown and "if (dpsmTimer)" in stop[timer:cancel], \
        "VF stop must null-check and cancel native DPSM timer before shutdown"
    assert "getMember<uint32_t>(that, 0x1458) =" not in stop and "waitForGpuIdle" not in stop, \
        "VF timer cancellation must not forge idle state or enter physical idle wait"
    quiesce = stop.index("vfQuiesceDeviceForShutdown(gVfHardwareGuc)")
    bridge_disable = stop.index("vfInterruptBridgeDisable", quiesce)
    if not quiesce < bridge_disable:
        raise AssertionError(
            f"{path}: final VF DMA quiescence no longer precedes bridge disable")
    if stop.index("vfInterruptBridgeDisable") > stop.index(
            "ioGraphicsDisableAccelerator"):
        raise AssertionError(f"{path}: VF stop lifecycle order is reversed")

    accelerator_stop = function_body(
        source, "void Gen11::acceleratorStop(void *that, void *provider)")
    stopping = accelerator_stop.index(
        "OSCompareAndSwap(0, 1, &gVfDeviceStopping)")
    original_stop = accelerator_stop.index(
        "FunctionCast(acceleratorStop, callback->oAcceleratorStop)(that, provider)")
    if not stopping < original_stop:
        raise AssertionError(
            f"{path}: native stop begins before the VF device-stopping boundary")

    normalized_production = "".join(source.split())
    for token in ("schedulerWait-schedulerInit!=0xc2",
                  "NGVfGuCFactoryPatch::schedulerInitPreflight(reinterpret_cast<constuint8_t*>(schedulerInit),0xaf)",
                  "schedulerInitPatch.apply(patcher,schedulerInit,0xaf)",
                  "NGVfGuCFactoryPatch::schedulerInitFreeFind",
                  "NGVfGuCFactoryPatch::schedulerInitFreeReplace"):
        assert token in normalized_production, "missing bounded scheduler failed-init production patch contract"
    assert normalized_production.index("NGVfGuCFactoryPatch::schedulerInitPreflight(") < \
        normalized_production.index("schedulerInitPatch.apply("), \
        "scheduler uniqueness admission must precede patch writes"
    accelerator_start = function_body(source, "bool Gen11::start(void *that, void *provider)")
    init_guard = function_body(source, "bool Gen11::vfInitScheduler(void *scheduler, uint32_t options,")
    native_init = init_guard.index("FunctionCast(vfInitScheduler, callback->originalSchedulerInit)")
    timer_load = init_guard.index("getMember<IOTimerEventSource *>(scheduler, 0x448)")
    expected_load = init_guard.index("getMember<IOWorkLoop *>(accelerator, 0xf0)")
    attached = init_guard.index("timer->getWorkLoop()")
    failed = init_guard.index('vfMarkProtocolFault("VF scheduler timer failed workloop attachment")')
    assert native_init < timer_load < expected_load < attached < failed < init_guard.index("return true;"), \
        "VF base scheduler init must validate attachment after native init before admission"
    assert "if (!timer || !expected)" in init_guard and "if (attached && attached != expected)" in init_guard and \
        'PANIC("ngreen", "Cannot release VF scheduler with foreign timer binding")' in init_guard and \
        "->free(" not in init_guard and "->release(" not in init_guard, \
        "VF init binding failure must leave factory cleanup ownership intact"
    assert '{"__ZN11IGScheduler15initWithOptionsEjyP22IOGraphicsAccelerator2",vfInitScheduler,this->originalSchedulerInit}' in "".join(pci_resolution.split()), \
        "missing typed base scheduler initialization route"
    create_guard = function_body(source, "void *Gen11::vfCreateScheduler(void *accelerator)")
    selection = create_guard.index("(getMember<uint32_t>(accelerator, 0x1190) >> 23) & 7U")
    reject = create_guard.index("if (schedulerType != 4U)", selection)
    fault = create_guard.index('vfMarkProtocolFault("VF final native scheduler selection is not GuC type 4")', reject)
    rejected = create_guard.index("return nullptr;", fault)
    delegation = create_guard.index("FunctionCast(vfCreateScheduler, callback->originalSchedulerCreate)")
    assert selection < reject < fault < rejected < delegation, "VF factory must reject non-GuC type before native dispatch"
    normalized_routes = "".join(pci_resolution.split())
    assert '{"__ZN11IGScheduler6createEP16IntelAccelerator",vfCreateScheduler,this->originalSchedulerCreate}' in normalized_routes, \
        "missing typed native scheduler factory admission route"
    options_guard = accelerator_start.index('IORegistryEntry::fromPath("IODeviceTree:/options")')
    options_copy = accelerator_start.index('options->copyProperty("GraphicsSchedulerSelect")', options_guard)
    options_type = accelerator_start.index('OSDynamicCast(OSData, overrideProperty)', options_copy)
    property_release = accelerator_start.index("OSSafeReleaseNULL(overrideProperty);", options_type)
    options_release = accelerator_start.index("OSSafeReleaseNULL(options);", options_type)
    options_fault = accelerator_start.index('vfMarkProtocolFault("VF rejects late device-tree scheduler override")', options_release)
    options_return = accelerator_start.index("return false;", options_fault)
    assert options_guard < options_copy < options_type < property_release < options_release < options_fault < options_return < accelerator_start.index("vfBootstrapDirectGgtt()"), \
        "VF options override guard must release entry and reject before bootstrap"
    options_block = accelerator_start[accelerator_start.rfind("if (vfActive)", 0, options_guard):options_return]
    assert "if (vfActive) {" in options_block and "hasSchedulerOverride" in options_block, \
        "device-tree override admission must be VF-only"
    assert "options->setProperty" not in accelerator_start and "options->removeProperty" not in accelerator_start, \
        "VF admission must not mutate global device-tree options"
    firmware_guard = accelerator_start.index('if (vfActive && PE_parse_boot_argn("-disablegfxfirmware"')
    firmware_fault = accelerator_start.index('vfMarkProtocolFault("VF cannot disable mandatory GuC firmware scheduling")', firmware_guard)
    firmware_return = accelerator_start.index("return false;", firmware_fault)
    assert firmware_guard < firmware_fault < firmware_return < accelerator_start.index("vfBootstrapDirectGgtt()"), \
        "VF firmware-disable guard must fail before bootstrap/native scheduler side effects"
    legacy_reject = accelerator_start.index("kVfLegacyPageOwnershipFlag")
    ggtt_bootstrap = accelerator_start.index("vfBootstrapDirectGgtt()")
    configure = accelerator_start.index("callback->ioPciConfigureInterrupts)(")
    native_start = accelerator_start.index("FunctionCast(start, callback->ostart)")
    if not legacy_reject < ggtt_bootstrap < configure < native_start:
        raise AssertionError(
            f"{path}: VF legacy-MMIO rejection/GGTT/MSI order changed before native start")
    if "pciDevice, kIOInterruptTypePCIMessaged, 1, 1, 0" not in accelerator_start:
        raise AssertionError(f"{path}: VF MSI request is not exactly one required vector")
    native_result = accelerator_start.index(
        "const auto result = FunctionCast(start, callback->ostart)(that, provider)")
    firmware_live = accelerator_start.index(
        "gVfSchedulerFirmwareReady && !gVfDeviceStopping", native_result)
    rollback = accelerator_start.index("acceleratorStop(that, nullptr)", firmware_live)
    quiesced = accelerator_start.index("!gVfDmaQuiesced", rollback)
    start_fault = accelerator_start.index(
        "native accelerator start failed after VF bootstrap", quiesced)
    if not native_result < firmware_live < rollback < quiesced < start_fault:
        raise AssertionError(
            f"{path}: live post-engine native-start failure is not DMA-quiesced before fault")

    for wrapper in ("bool Gen11::wrapIGScheduler5IsGpuIdle(const void *that)",
                    "bool Gen11::wrapIGScheduler4IsGpuIdle(const void *that)"):
        if "return vfKnownIdleSnapshot();" not in function_body(source, wrapper):
            raise AssertionError(
                f"{path}: DPSM idle route no longer uses VF context state")

    late_callback = function_body(
        source, "void Gen11::vfRequestEnableCallback(void *that, OSObject *requestor,")
    enabled = late_callback.index("getMember<uint8_t>(that, 0x8A8)")
    immediate = late_callback.index("action(requestor)")
    original = late_callback.index("callback->oVfRequestEnableCallback)(")
    if not enabled < immediate < original:
        raise AssertionError(
            f"{path}: post-enable callback is not serviced before native queue fallback")

    quiesce = function_body(source, "bool vfQuiesceDeviceForShutdown(void *guc)")
    for token in (
            "if (!gVfCtbEverEnabled)",
            "vfDirectContextTableUnowned()",
            "OSCompareAndSwap(0, 1, &gVfDmaQuiesced)",
            "OSCompareAndSwap(0, 1, &gVfContextShutdownComplete)"):
        if token not in quiesce:
            raise AssertionError(f"{path}: incomplete pre-CTB rollback proof: {token}")

    unowned = function_body(source, "bool vfDirectContextTableUnowned()")
    for token in (
            "entry.state != kVfGucContextEmpty",
            "entry.contextBacking",
            "entry.refCount",
            "entry.enablePending",
            "entry.disablePending"):
        if token not in unowned:
            raise AssertionError(
                f"{path}: incomplete direct-context ownership proof: {token}")

    failed_bootstrap = function_body(
        source, "bool vfRollbackFailedPostCtbBootstrap(void *guc)")
    for token in (
            "guc != gVfHardwareGuc",
            "gVfCtbEverEnabled",
            "gVfSchedulerFirmwareReady",
            "gVfProtocolFault",
            "gVfMmioPoisoned",
            "vfCloseContextOperationGateAndWait(guc)",
            "vfDirectContextTableUnowned()",
            "vfCloseIrqCallbackGateAndWait(guc)",
            "kGucActionHost2GucControlCtb, 0",
            "NGVfMmioResponse::noData(reply[0])",
            "OSCompareAndSwap(0, 1, &gVfDmaQuiesced)"):
        if token not in failed_bootstrap:
            raise AssertionError(
                f"{path}: incomplete failed post-CTB rollback proof: {token}")

    mmio_start = source.index(
        "bool Gen11::vfMmioHostToGuCAction(void *that, const uint32_t *request,")
    configured = source.index("vfConfigureModernCtb(", mmio_start)
    consume = source.index("vfConsumeMemoryInterrupts()", configured)
    drain = source.index("pollVfGuCToHost(that)", consume)
    response = source.index("NGVfLegacyCtb::responseStatus(ok)", drain)
    if not configured < consume < drain < response:
        raise AssertionError(
            f"{path}: CTB enable-boundary drain is not ordered before success")
    for signature in (
            "bool vfInvalidateTLBSync(void *guc)",
            "bool vfWaitForContextState(void *guc, uint16_t gucId, VfGucContextState wanted)",
            "bool vfWaitForContextTransition(void *guc, uint16_t gucId, uint32_t lrcaPage,"):
        if "pollVfGuCToHost(guc)" not in function_body(source, signature):
            raise AssertionError(f"{path}: synchronous GuC wait lacks bounded G2H polling: {signature}")

    bootstrap_abort = function_body(
        source, "static void vfAbortSchedulerBootstrap(const char *reason)")
    if ("gVfSubmissionStopped" not in bootstrap_abort or
            "vfMarkProtocolFault" in bootstrap_abort or
            "gVfProtocolFault" in bootstrap_abort):
        raise AssertionError(
            f"{path}: local bootstrap abort poisons its required teardown transport")
    create = function_body(
        source, "uint32_t Gen11::vfCreateUkContext(void *that, uint64_t owner, int priority)")
    for token in (
            "using GetMemory = void *(*)(void *)",
            "using GetPhysicalSegment = uint64_t (*)(void *, uint64_t, uint64_t *)",
            'metaCast("IGAccelSysMemory")',
            "callback->vfAccelSysMemoryGetPhysicalSegment",
            "vfAbortSchedulerBootstrap(\"invalid VF proxy process physical segment\")"):
        if token not in create:
            raise AssertionError(f"{path}: missing exact proxy DMA ABI/rollback token {token}")
    for forbidden in ("memory->getPhysicalSegment", "kIOMemoryMapperNone"):
        if forbidden in create:
            raise AssertionError(
                f"{path}: proxy DMA lookup reintroduced wrong ABI {forbidden}")
    workqueue = function_body(
        source, "bool Gen11::vfWorkQueueInit(void *that, void *accelerator, uint32_t id, void *process)")
    if workqueue.index("vfAbortSchedulerBootstrap(") > workqueue.index(
            "NGWorkQueue::unwindFailedInit"):
        raise AssertionError(
            f"{path}: failed workqueue releases mappings before stopping producers")
    unmap = function_body(
        source, "void Gen11::IGHardwareGlobalPageTableUnmapRange(void *that,")
    if "gVfCtbEverEnabled && gVfProtocolFault && !gVfDmaQuiesced" not in unmap:
        raise AssertionError(
            f"{path}: pre-CTB protocol failure cannot unwind DMA-free mappings")
    ggtt_postwrite_contract(source, path)
    print(f"PASS: VF wrapper preserves native bridge/IOAccel lifecycle in {path}")


def ggtt_postwrite_contract(source, path="<source>"):
    ggtt_barrier = function_body(source, "static void vfRequireCompletedGgttUpdate()")
    if 'PANIC_COND(!vfCompleteGgttUpdate(), "ngreen",' not in ggtt_barrier:
        raise AssertionError(f"{path}: failed post-write GGTT invalidation returns into backing cleanup")
    for signature, expected in (
            ("bool Gen11::IGHardwareGlobalPageTableMapRange(void *that,", 1),
            ("bool Gen11::IGHardwareGlobalPageTableMapRangeRotated(void *that,", 2),
            ("bool Gen11::IGHardwareGlobalPageTableMapRangeDummy(void *that,", 1)):
        body = function_body(source, signature)
        if body.count("vfRequireCompletedGgttUpdate();") != expected or "vfCompleteGgttUpdate()" in body:
            raise AssertionError(f"{path}: GGTT mapping bypasses checked post-write completion: {signature}")
        first_write = body.index("pteBase[")
        if body.index("vfRequireCompletedGgttUpdate();") < first_write:
            raise AssertionError(f"{path}: GGTT completion barrier precedes PTE writes")
    rotated_map = function_body(source, "bool Gen11::IGHardwareGlobalPageTableMapRangeRotated(void *that,")
    rollback = rotated_map[rotated_map.index("const uint64_t dummyPte"):]
    if rollback.index("vfRequireCompletedGgttUpdate();") > rollback.index("segments->memory->release();"):
        raise AssertionError(f"{path}: failed GGTT rollback drops descriptor before completion")
    final_map = rotated_map[rotated_map.index("rotated->sourcePage ="):]
    if final_map.index("vfRequireCompletedGgttUpdate();") > final_map.index("segments->memory->release();"):
        raise AssertionError(f"{path}: rotated mapping drops descriptor before completion")


def ggtt_postwrite_mutations(path):
    source = pathlib.Path(path).read_text()
    mutations = (
        ('PANIC_COND(!vfCompleteGgttUpdate()', 'PANIC_COND(vfCompleteGgttUpdate()'),
        ('vfRequireCompletedGgttUpdate();', '(void)vfCompleteGgttUpdate();'),
        ('\t\tvfRequireCompletedGgttUpdate();\n\t\tsegments->memory->release();',
         '\t\tsegments->memory->release();\n\t\tvfRequireCompletedGgttUpdate();'),
        ('\tvfRequireCompletedGgttUpdate();\n\tsegments->memory->release();',
         '\tsegments->memory->release();\n\tvfRequireCompletedGgttUpdate();'),
    )
    for before, after in mutations:
        assert before in source, "missing GGTT mutation anchor"
        try:
            ggtt_postwrite_contract(source.replace(before, after, 1))
        except AssertionError:
            continue
        raise AssertionError("GGTT post-write contract accepted unsafe mutation")
    print("PASS: four GGTT post-write guard/release-order mutations rejected (source contract, not DMA proof)")


def event_collection_admission_model():
    """Selected collector specification only; NOT a live owner/admission gate.

    Native body fixtures separately pin the reviewed branch graph. Snapshot
    addresses here are symbolic stable inputs, not evidence of runtime leases.
    """
    def snapshot_matches(expected, observed):
        # Selected consumer dereferences entries without a null guard. This
        # rejects null, but does not validate pointer ownership or accessibility.
        return all(value != 0 for vector in expected for value in vector) and expected == observed

    cases = 0
    for client_copy in (False, True):
        for skip_wait in (False, True):
            for pair_events in (False, True):
                for mapped in (False, True):
                    for aliased in (False, True):
                        wait, update = [], []
                        # Include two resources, which can legitimately alias
                        # event storage. Preserve multiplicity, never dedup.
                        for resource in range(2):
                            base = 0x10000 + (0 if aliased else resource * 0x1000)
                            if pair_events:
                                update.extend((base + 0x900, base + 0x940))
                            else:
                                if not (client_copy and skip_wait):
                                    wait.append(base + 0x100)
                                update.extend((base + 0x100, base + 0x140))
                            if mapped:
                                update.append(base + 0xa38)
                        expected = (tuple(wait), tuple(update))
                        required = len(wait) + len(update)
                        # Superset of allocation omissions: every possible
                        # missing-entry set must reject before publication.
                        for mask in range(1 << required):
                            observed = (
                                tuple(value for i, value in enumerate(wait) if mask & (1 << i)),
                                tuple(value for i, value in enumerate(update, len(wait)) if mask & (1 << i)))
                            complete = snapshot_matches(expected, observed)
                            assert complete == (mask == (1 << required) - 1)
                            assert not complete or sum(map(len, observed)) == required
                            cases += 1
                        for vector in (0, 1):
                            if expected[vector]:
                                substitution = list(expected[vector])
                                substitution[-1] = 0xdead0000
                                bad = list(expected)
                                bad[vector] = tuple(substitution)
                                assert not snapshot_matches(expected, tuple(bad))  # same count is insufficient
                                assert tuple(map(len, bad)) == tuple(map(len, expected))
                        assert len(wait) == (0 if pair_events or (client_copy and skip_wait) else 2)
                        assert len(update) == (6 if mapped else 4)
    # Genuine no-op selection is distinct from all required events omitted.
    selected_noop = ((), ())
    omitted_required_wait = ((0x100,), ())
    assert snapshot_matches(selected_noop, selected_noop)
    assert not snapshot_matches(omitted_required_wait, selected_noop)
    for expected in (((0,), ()), ((), (0,)), ((), (0, 0))):
        assert not snapshot_matches(expected, expected)  # exact identity alone is insufficient
    print(f"PASS: {cases} selected event-omission states; legal skip/alias and same-count substitution (offline model, not runtime admission)")


def main():
    if len(sys.argv) != 4:
        raise SystemExit(f"usage: {sys.argv[0]} kern_gen11.cpp TGL-production TGL-debug")
    direct_branch_candidate_contract()
    event_collection_admission_model()
    source_contract(sys.argv[1])
    ring_backing_submit_mutations(sys.argv[1])
    g2h_event_transaction_mutations(sys.argv[1])
    g2h_completion_lock_mutations(sys.argv[1])
    ggtt_postwrite_mutations(sys.argv[1])
    macho_inventory(sys.argv[2])
    macho_inventory(sys.argv[3])


if __name__ == "__main__":
    main()

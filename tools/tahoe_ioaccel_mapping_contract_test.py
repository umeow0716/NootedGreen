#!/usr/bin/env python3
"""Check a locally archived Tahoe KC; not a GPU completion/safety test."""

import hashlib
import pathlib
import struct
import sys


KC_SHA256 = "5cb1be1dc530b4b953a33943567589101d3ac46bb8cf90728566ee7e5b1fa214"
BOOT_SHA256 = "5cba9e36ceed5d73e1d569d1772bc46fecbd0359f824db689863e686d856ea3b"
IDENTIFIER = b"com.apple.iokit.IOAcceleratorFamily2"
IOACCEL_UUID = bytes.fromhex("6c4a16b166dc3a8b9f0bd3e027b16eb1")
EVENT_VTABLE = "__ZTV24IOAccelEventMachineFast2"
EVENT_FINISH = "__ZN24IOAccelEventMachineFast211finishEventEP12IOAccelEvent"
EVENT_WAIT = "__ZN20IOAccelEventMachine212waitForStampEijPj"
EVENT_CLEAN = "__ZN24IOAccelEventMachineFast210cleanEventEP12IOAccelEvent"
EVENT_TERMINATE = "__ZN24IOAccelEventMachineFast224deviceTerminatedUnlockedEv"
EVENT_SIGNAL = "__ZN20IOAccelEventMachine211signalStampEij"
EVENT_RESTART = "__ZN20IOAccelEventMachine215restart_channelEv"
EVENT_MERGE_EXCLUDING = "__ZN24IOAccelEventMachineFast219mergeEventExcludingEP12IOAccelEventS1_i"
EVENT_SET_STAMP = "__ZN24IOAccelEventMachineFast213setEventStampEiP12IOAccelEvent"
EVENT_INCREMENT = "__ZN24IOAccelEventMachineFast214incrementStampEi"
EVENT_WRITE_STAMP = "__ZN24IOAccelEventMachineFast217writeStampCommandEiP17IOAccelEventQueueP17vendevtCommandRec"
EVENT_SCRUB = "__ZN24IOAccelEventMachineFast210scrubEventEP12IOAccelEvent"
SHARED_SCRUB = "__ZN14IOAccelShared211scrubEventsEv"
RESOURCE_SCRUB = "__ZN16IOAccelResource211scrubEventsEv"
SHARED_VTABLE = "__ZTV14IOAccelShared2"
RESOURCE_VTABLE = "__ZTV16IOAccelResource2"
GET_DATA_BUFFER = "__ZN15IOAccelContext213getDataBufferEP29IOAccelContextGetDataBufferInP30IOAccelContextGetDataBufferOutP22IOAccelResourcePrivatey"
EVENT_FINISH_UNLOCKED = "__ZN24IOAccelEventMachineFast219finishEventUnlockedEP12IOAccelEvent"
EVENT_HARDWARE_ERROR = "__ZN20IOAccelEventMachine219signalHardwareErrorE15eRestartRequesti"
EVENT_INIT = "__ZN24IOAccelEventMachineFast29initEventEP12IOAccelEvent"
EVENT_COPY = "__ZN24IOAccelEventMachineFast29copyEventEP12IOAccelEventS1_"
EVENT_DISABLE_STAMP_LOCKED = "__ZN20IOAccelEventMachine223disable_stamp_interruptEi"
EVENT_ENABLE_STAMP = "__ZN20IOAccelEventMachine220enableStampInterruptEi"
EVENT_DISABLE_STAMP = "__ZN20IOAccelEventMachine221disableStampInterruptEi"
EVENT_OWNER_BODIES = {
    "__ZN17IOAccel2DContext211set_surfaceEj23eIOAccelContextModeBits": (0x338, "eb383c47336521fc9c7c55b7bb1f979bc55e51d23e582390434f92a4883defff"),
    "__ZN17IOAccel2DContext26finishEj": (0x158, "4d59163fe136e649136c614f200b425e1807f1c2c8b5884ed46019afdba8b320"),
    "__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy": (0x7ae, "2fcbf2f864a5133910c0cdd7096d0464e9fa2787ef1d079a7c2fa8fbf172307e"),
    "__ZN17IOAccel2DContext212contextStartEv": (0xa0, "8f77114e552abeffa9a366a0d478a4870b0737c8e3406378c15030484e5e0138"),
    "__ZN17IOAccel2DContext211contextStopEv": (0x9e, "75a96e57df086bab884b5928e4d12e3a3ab3c3ff1b552310727d167a91944145"),
    "__ZN17IOAccel2DContext226getTargetAndMethodForIndexEPP9IOServicej": (0x36, "f7a9b91b57ca910960fcc84026c9880b649e210756519ba3476a0a37f0da40da"),
    "__ZN15IOAccelContext219submit_data_buffersEP33IOAccelContextSubmitDataBuffersInP34IOAccelContextSubmitDataBuffersOutyPy": (0x972, "d79b1848b67a97bdf24ef16ce5b76f4431ede6500bd4b99afd04a6b691f1c0bb"),
    "__ZN15IOAccelContext210stopLockedEv": (0x52, "a5b99d2db4fbb23a56228962f8b641d7495ed916bba2c91096ab30cd30ea6c54"),
    "__ZN15IOAccelContext24stopEP9IOService": (0xd4, "e3a5af9de5bf44f29f60a0a795a754c1bfb80123915ed8ae6e6b2fe52e6e8552"),
    "__ZN15IOAccelContext212contextStartEv": (0x424, "9b5da3ec12301b8ec5b7efe54362371aa8a6fd91e2d6105ed0c123dd6d104175"),
    "__ZN15IOAccelContext211contextStopEv": (0x278, "0544bbbb0d2e84ffa6e65365d43fc1b82ee588ab5ab0ed1e30d812597391684c"),
    "__ZN15IOAccelContext218processDataBuffersEj": (0x1b8, "17cc659d6536950bbcf5773445a36a3c74982437500f07e026057d34c6fa0a73"),
    "__ZN15IOAccelContext226getTargetAndMethodForIndexEPP9IOServicej": (0x4c, "aee1728e8f66dc533ec275c689f4da0a5946787357ab0565ae7c7863f8562b6b"),
    "__ZN19IOAccelCommandQueue24s_submit_command_buffersEPS_PvP25IOExternalMethodArguments": (0x130, "a52d207321a36401036ea55030dec2f107d78189f6822dbc688f715e18f85432"),
    "__ZN19IOAccelCommandQueue10stopLockedEv": (0xae, "a72a67b078ee8dd715efe31a50ae55064ab77274521e7ca872b54f9feff211ec"),
    "__ZN19IOAccelCommandQueue4stopEP9IOService": (0x96, "be5aa23dab120c8477bdd1fcf08f3b4b7090c94ce72248c7cb0cdd3f9ef78eea"),
    "__ZN19IOAccelCommandQueue22submit_command_buffersEPK29IOAccelCommandQueueSubmitArgs": (0x394, "e5fcf4650c57a73f6761d81b9ff4ac63e1fdfb76046026e5d08bb93a03d5056e"),
    "__ZN19IOAccelCommandQueue21submit_command_bufferEjjyy": (0x26e, "1848221712bd1f1a5dc9e642b6c62308f6d47c60ca0f74d3f67e0a2f3d09a0c3"),
    "__ZN19IOAccelCommandQueue20processCommandBufferEjj": (0x590, "5665608a9c122e75fd821c2874eec220890daf988dd9bc337019d5ba6b7823e5"),
    "__ZN19IOAccelCommandQueue22process_command_bufferEjj": (0x580, "f5c959f48d3cae9fb9498b5336510a8775b47b01b2609bf0df26aee08030fa90"),
    "__ZN19IOAccelCommandQueue22releaseAcceleratorLockEv": (0x62, "ffe0e06201741cc7921d0a758be1b5405cc01ce4736c65deeab7eef3b2c329a9"),
    "__ZN19IOAccelCommandQueue22acquireAcceleratorLockEv": (0x80, "4fb4e1e4d21a05672a06ac10548d48087309343b1dcab7670b762f8e6316dfa4"),
    "__ZN22IOGraphicsAccelerator222acceleratorWaitEnabledEv": (0xbc, "548ab4a104556a0907dbbf4939411fca5bbe55373c51b1bc916826eb0c03bb5a"),
    "__ZN19IOAccelCommandQueue27shouldHardwareCommandNoopedEv": (0x8, "5b1ee01dedea0fcb3ecce46b2207fda587d289c2e69a0afdb9d54f2595762f2e"),
    "__ZN19IOAccelCommandQueue22canSubmitCommandBufferEv": (0x8, "aaa500a73706124bc5374dc27c8b444160b15dc8a45b0fef9354b23106b76348"),
    "__ZN19IOAccelCommandQueue24pauseSubmitCommandBufferEv": (0xf, "ccf6e7e6a202c29bdb7f560fbd89d522c30456c3ec0bf18760b66c97c3f1ac94"),
    "__ZN15IOAccelContext222canSubmitCommandBufferEv": (0x8, "aaa500a73706124bc5374dc27c8b444160b15dc8a45b0fef9354b23106b76348"),
    "__ZN15IOAccelContext224pauseSubmitCommandBufferEv": (0xf, "1a4e0dc116008dda5af49a0cb424eca0433ad07c36dbec572a19b439c87e7058"),
    "__ZNK25IOAccelCommandBufferPool29MetaClass5allocEv": (0x48, "9859ba4306233dba03cef99c8cc67a1001c7dc469e0fef1f93212822331ad712"),
    "__ZN25IOAccelCommandBufferPool2C1Ev": (0x30, "22f68369dac6b818a50084fdfa943033e3801c1b55f91858521f9c5cf77fbb36"),
    "__ZN28IOAccelCommandBufferPoolList14addCommandPoolEP25IOAccelCommandBufferPool2": (0x14, "dd5d756962a0d9d7bd2c5168492d6202cd17167529578338f639cf9abea4cdc4"),
    "__ZN28IOAccelCommandBufferPoolList17removeCommandPoolEP25IOAccelCommandBufferPool2": (0x9a, "fcc000816e19074bcb33ab0425fb6bca1615d6b623f8aa42f2d17759e6311c81"),
    "__ZN25IOAccelCommandBufferPool24initEP22IOGraphicsAccelerator2P15IOAccelChannel2P11IOAccelTaskiijjjj": (0x182, "58d38ff5fe772731cb08b042d75ce12ae46ea7cc046b78e7cda9b2d4865fc82c"),
    "__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv": (0x202, "32fc16f1a5c64764f3c81e4c0e2a95a65d6cdd9a3da934964e3e4db74e379f3b"),
    "__ZN25IOAccelCommandBufferPool24freeEv": (0x13c, "739b44800bc58c97f593876b5e102076a8b17adcdda60c512129fff840b580d3"),
    "__ZN25IOAccelCommandBufferPool217getBufferPtrNoIncEj": (0x10e, "bf2d3995728e15a07c3e8a2b0adebd4f8e9ecc063ee4767e44bc4c5e5763567a"),
    "__ZN25IOAccelCommandBufferPool221setBufferCurrentIndexEs": (0x182, "5cf69325a1d3fe2e3f09751af5ec3b3eece1542d255c6b04411d348e40c2f033"),
    "__ZN25IOAccelCommandBufferPool212setBufferPtrEPj": (0xe, "966161046c4b88de4a6eebbb532da658c7a201330fb194055973f8af790dd35f"),
    "__ZN25IOAccelCommandBufferPool212submitBufferEv": (0x186, "148ea39a6e655b0db0ef181a37165ebd35bb92ab0bf72607001484e420697602"),
    "__ZN18IOAccelDisplayPipe38set_current_plane_ioSurfaceDeviceCacheEP12IOAccelEventjjP20IOSurfaceDeviceCache": (0x21a, "8beabc3abe7e423ff49dacf19daa96567c69e70b727205ae1db92b8b4ed10ce5"),
    "__ZN30IOAccelDisplayPipeTransaction220set_transaction_argsEP33IOAccelDisplayPipeTransactionArgs": (0xab0, "830382a2686dc7615224b4b12d072e1484eef55f80854c6f017ffae6b9d1ceb2"),
    "__ZN13IOAccelMemory19createMappingInTaskEP11IOAccelTaskj": (0x16, "47b5beedeaa9f97ff450a1c3e79647872b26fbd32afe277c8638f9016202bac1"),
    "__ZNK16IOAccelResource210getGPUTaskEv": (0x22, "7dc3a618b56c2611b1d27a58ce28260cd354c9d58275fb75b084e23e31fe0b3a"),
    "__ZN11IOAccelTask4freeEv": (0x144, "e03bbd00acba08d6610d5a07d51fd15f52a3f96925399177e6f5d61599dbad23"),
    "__ZN15IOAccelTaskList10removeTaskEP11IOAccelTask": (0x9a, "9f9a37a7142d6594debe3200a77b7aacbaaaa9ad8dd55df2c490f0cbf232267b"),
    "__ZN22IOGraphicsAccelerator218freeAllGPUMappingsEv": (0xba, "55e1bb60b897503d7fd25ee08668ca0caa985914b1a0735cce97aab7c726548e"),
    "__ZN24IOAccelSharedUserClient25startEP9IOService": (0xea, "0daca245b77c7a9be1d589e35170d3b9c99952365780f0f98d66bcd548b95cb8"),
    "__ZN24IOAccelSharedUserClient211sharedStartEv": (0x62, "31735fd69d8d3860bbc5cc1cbef7e669bdfbfaa97213af6d14b237fd434208e7"),
    "__ZN22IOGraphicsAccelerator212createSharedEP4task": (0x50, "7256dbd56b27e4f81d558ece49ba7614a28ad69024c57f3b07fc602615be87b8"),
    "__ZN14IOAccelShared24initEP22IOGraphicsAccelerator2P4task": (0x208, "6c3bf31fd74c2c0066ef53a4597b640a967bb96a8b2a974a1a88a81fe217b20a"),
    "__ZN11IOAccelTask4initEP22IOGraphicsAccelerator2jPP16IORangeAllocator": (0x138, "f00989c4635c6bfcba66ac1d775dfb16259703e55064355a7686bb3e9db9e186"),
    "__ZN15IOAccelTaskList7addTaskEP11IOAccelTask": (0x14, "dd5d756962a0d9d7bd2c5168492d6202cd17167529578338f639cf9abea4cdc4"),
    "__ZN24IOAccelSharedUserClient215delete_resourceEj": (0x116, "3284f01291ec4103e1a0695dcad54bdc3722cdb8a303c867459b7e3cce0cb302"),
    "__ZN16IOAccelMemoryMap15remove_resourceEP16IOAccelResource2": (0x5e, "72e6f743a75a66e9d29bfa658f891e0c5c99e462a9fe3a740c44deef0eb068ea"),
    "__ZN16IOAccelResource24freeEv": (0x41c, "ed792391afedafbd9924dc5d2bdc447d0b116644f8e5b2f34161398e982c79b4"),
    "__ZN16IOAccelResource210initializeEP22IOAccelNewResourceArgsy": (0x33e, "7d083706bf76b368690668e896e04d2f33c89bd906457e32e0699a283aa70c1a"),
    "__ZN16IOAccelResource210checkDirtyEv": (0x144, "9e26e53e3aa25e3a0d7c81a710ca44d4fb0a8081b31d186bb04566f618af5a64"),
    "__ZN16IOAccelResource212addToChannelEP15IOAccelChannel2j": (0x22a, "696a8c305a8d69ce7573addf9147eae02e4c4055bd231d9b433f88edec44631c"),
    "__ZN14IOAccelShared214lookupResourceEjPPv": (0xe, "839e3c8f1d2321d46fab9a7a166f590702861f015fe827f73d1ffd2b4e4ce8b7"),
    "__ZNK16IOAccelNamespace8lookupIdEjPPv": (0x2a, "1f00cff53025fc424b77a62fde2f5d9357dc200a4604f1bc5edcd95a6aad6f4a"),
    "__ZN16IOAccelResource217removeFromChannelEP15IOAccelChannel2": (0x14c, "6c564440b5d53195a0a352e8bddd4516e114a533aa5facb11f0de6c35a1b73ba"),
    "__ZN22IOGraphicsAccelerator217system_will_sleepEib": (0x25c, "0e551a9a306fff5cfb8d9fad02635355ce639e4a28cb2635687da8253d07f580"),
    "__ZN22IOGraphicsAccelerator215systemWillSleepEv": (0x190, "0ed3b3ceeb3fdab4a219b59e50049aa79b8c2b126252413e682c6b2b1aa7641d"),
    "__ZN22IOGraphicsAccelerator215system_did_wakeEib": (0x20c, "9859c2de3fbba2eee6dbd9eb6a5aeebc92cacf7b855e4bcc08c46de6c79d3ca0"),
    "__ZN15IOAccelTaskList8IteratorC1ERS_": (0xc, "b381fee4d716b47f962d55fec3af58dd61a81d635a5141be724b0af8f0287012"),
    "__ZN15IOAccelTaskList8Iterator11getNextTaskEv": (0x16, "7d2d9468414d7b35cd266d97b73da45c3a02d815e2f0d8a65674137e136ce023"),
    "__ZN24IOAccelSharedUserClient211clientCloseEv": (0x54, "c97b26393ef1c8b63b77659306adbd55b501f72dff0201c0384f5e02d82ca9ea"),
    "__ZN24IOAccelSharedUserClient24freeEv": (0x6a, "ad1419c125f7c469eae38203746a917ead22faf069eebb909121c85dd70dddd3"),
    "__ZN24IOAccelSharedUserClient24stopEP9IOService": (0xf4, "28f150a450f8d1a8a00677a7d69332cc0ca1b873ebe61ac3c12fedd37be25dbf"),
    "__ZN24IOAccelSharedUserClient210sharedStopEv": (0x46, "dab0e743927cb3b74cbed094abb998040275f4c86a9ddbf53bf358ddcff9ae4b"),
    "__ZN14IOAccelShared24freeEv": (0x2be, "5f5f01995a1f2b57eaab2d62109fa86cb1345ff459099eaf3502419cc42a4e59"),
    "__ZN25IOAccelOrphanedMemoryPool13sharedReleaseEP14IOAccelShared2": (0x9a, "2677894d4102600f9b8f05eb577d45efa015d334f75f13d227f26c60e2f91140"),
    "__ZN16IOAccelResource213sharedReleaseEP14IOAccelShared2": (0xcc, "e17e5a1f423425d6b0c68a263e756d7295cdde33bce3838acb02475b3c6d8be0"),
    "__ZN11IOAccelTask8allocateEPK16IOAccelMemoryMap": (0xc8, "861b200bb16c1a1c269a89bcbe9b903b185aae1a790818b603f7eacdcc5bf4fb"),
    "__ZN11IOAccelTask10deallocateEPK16IOAccelMemoryMapy": (0x6e, "9b10f7f24edcb00e45e9a15816d4dea1ec5467202c7f5efc6681b71d1bae6170"),
    "__ZN11IOAccelTask4initEP22IOGraphicsAccelerator2jPP16IORangeAllocator": (0x138, "f00989c4635c6bfcba66ac1d775dfb16259703e55064355a7686bb3e9db9e186"),
    "__ZN13IOAccelMemory34createMappingInTaskAtAddressLengthEP11IOAccelTaskjyy": (0x272, "0f88152129c26add226c33d981c05fa0a0f3f4fa14c3655446872be8ac308758"),
    "__ZN16IOAccelMemoryMap22allocGPUVirtualAddressEv": (0x6a, "3e5d8e2802624a6e6c3122ba4a5767c26ac6e7c7d257f992c2e436745acf0549"),
    "__ZN16IOAccelMemoryMap24reserveGPUVirtualAddressEyy": (0x68, "9fec19ea44c53e447b4aa03d6f548f80ce78b60048e8ff6521b8aaecb7476583"),
    "__ZN16IOAccelMemoryMap21freeGPUVirtualAddressEv": (0x6c, "2ad700b816aed108e396bdc25f75919cb459af88a69b3b427704db1f1af6b3f6"),
    "__ZNK11IOAccelTask7releaseEv": (0x1a2, "a3e402420b875e449bce460ff8e7efc7756406af7c6ad2cebf12f18ccd37cdad"),
    "__ZN22IOGraphicsAccelerator222free_orphaned_gputasksEv": (0x8c, "94abdc9982851a426b754470c26e1bb1040000a6ddda2262262d809c614f44fe"),
    "__ZN22IOGraphicsAccelerator223kickOrphanResourceTimerEv": (0x3c, "a82728319f96b77e8783a066eb6db8b23b9bfa77d8c1a2711fec61bb459973ef"),
    "__ZN22IOGraphicsAccelerator217garbage_collectorEP22IOInterruptEventSourcei": (0xce, "61f516a20d099fa5cec14c3e5a2d3e94068319aab6ba6e74511e2faa25f7fbd5"),
    "__ZN22IOGraphicsAccelerator214gart_collectorEP22IOInterruptEventSourcei": (0x1d4, "c8340782d1db7b9496bae81aadee5664d2697480265cb02c1e6e32349601b047"),
    "__ZN11IOAccelTask22free_orphaned_mappingsEv": (0x7e, "97d743c58e34dfba9f18849fa65ffe915d28af60f8b2128c8a94ce5a771bda5b"),
    "__ZN11IOAccelTask23prune_orphaned_mappingsEv": (0x82, "27fb718099f7605e54763ca2c0f847341ee269eac72bd41ebf21a93d73d812de"),
    "__ZN11IOAccelTask18freeAllGPUMappingsEv": (0xce, "73823442676f252f9be50c157c8d4a87e8b012e255d9c33a9e1d647f59e8fa66"),
    "__ZN16IOAccelMemoryMap11finishEventEv": (0x26, "9f63366c69a3921beecdf3f17fd6442c3a164b13c58271a98c3493cd16c2eb03"),
    "__ZN16IOAccelMemoryMap9testEventEv": (0x26, "ba2d03cf31f94fead1d82e2957c392db5b512b18d18658198c9a7b38bc1d6b6c"),
    "__ZN20IOAccelMemoryMapList15ReverseIteratorC1ERS_": (0xe, "efa5c4b0f81f739fa8e47eacdc469e74a2b65b2c9239f19b2ced366ebb62607b"),
    "__ZN20IOAccelMemoryMapList15ReverseIterator14getPrevMappingEv": (0x16, "e6f7f96d180c0e35feee238564b6a24025becd983bb363e01165717bfcfd25a4"),
    "__ZN24IOAccelEventMachineFast29testEventEP12IOAccelEvent": (0x3e, "6a3da94f7b063da6967b9804bde524c225da4135b7fb11ec4e80cda03671d193"),
    "__ZN24IOAccelEventMachineFast217testEventUnlockedEP12IOAccelEvent": (0x6c, "cb4859871ba1903a1f25b38ca9f29f8502d2627794aa45624d0fb85291e66e40"),
    "__ZNK16IOAccelMemoryMap7releaseEv": (0xfe, "1b2bdc6aa5b9cf5b153d22ee8509b4293033285bab3cf297179f87b73fb08691"),
    "__ZN20IOAccelMemoryMapList13removeMappingEP16IOAccelMemoryMap": (0x7c, "0da924d403ae611e17fc1e2d9bb1b35e17d8a3c27a44a88418fc54702fb0420b"),
    "__ZN20IOAccelMemoryMapList10addMappingEP16IOAccelMemoryMap": (0x2a, "e142938a7033edaef0ef5446d522d5fc627a8c8224c60061d81f529957a2392b"),
    "__ZN13IOAccelMemory18check_orphan_stateEv": (0x42, "8fa89f21351705d925cc4d43426c46a92bb99ba85c99935d27c280f91d1ef7a9"),
    "__ZN16IOAccelMemoryMap10commit_pteEv": (0x64, "648a1ffd7c72e238dc0e8ad6cc99ad5d157687c64ba6531eed2424b5d4065574"),
    "__ZN22IOGraphicsAccelerator212sysmem_wiredEP16IOAccelSysMemory": (0x78, "2af8bdaad7f93f070852b210c6594612b4830c13f317b6c0606f09ea551338e6"),
    "__ZN24IOAccelResidentMemorySet9addMemoryEP13IOAccelMemory": (0x64, "40149e4f9e96697bf965b03d5786ab77eb8e52ab9c5e963632233f31ce5b9131"),
    "__ZN24IOAccelResidentMemorySet12removeMemoryEP13IOAccelMemory": (0x6c, "3abb8f4eeb8d8b0cacac8d433d219dae87346b2d6fdaa38187ae423fad7b51e6"),
    "__ZN24IOAccelResidentMemorySet11LRUIteratorC1ERS_": (0x1e, "61c65d6c7456b61ef1def643f5665beced80dbdaa58362a38b694541cb289168"),
    "__ZN24IOAccelResidentMemorySet11LRUIterator13getNextMemoryEv": (0x70, "fb7ff01ffa4671534fe423cc5c1db1d5e84fe7d1ccd19018c5c46e4b647908f7"),
    "__ZN24IOAccelResidentMemorySet4sortEv": (0x1c0, "d596290abea11b8c1d45b30a1a71bf5312bead39d4d14716f48b770bfae6ddbe"),
    "__ZN24IOAccelResidentMemorySet7reallocEv": (0x7a, "f724e67795dc28b0325846e2df5627def50c7ce190563e3260199464bd0c557c"),
    "__ZNK13IOAccelMemory10getLRUSeedEv": (0x56, "78a25180c308e0672b27a0967f988508dd9c624f1b28cd506323c84003060fd8"),
    "__ZN22IOGraphicsAccelerator220freeToPrepareMappingEP16IOAccelMemoryMap": (0x260, "8706a375f2e2538da76435f0f768ac1a42afc03ef111330ba5d09d51ac29973b"),
    "__ZN22IOGraphicsAccelerator223freeWaitToPrepareSysMapEP16IOAccelMemoryMapb": (0x1bc, "d36fa6823dfe656c5735419f5c48261954ee0a397e670822e2875197baad0bf5"),
    "__ZN16IOAccelSysMemory8completeEv": (0x42, "37fec16c6bebb121abb7612a23dbd90ccdae8179f025c1edae4e7553dc779240"),
    "__ZN16IOAccelMemoryMap7prepareEv": (0x7c, "e2f18c8db02cdedfce50fc42a2c86c3e64b7c8d4bd4baacde07d0dfdb90858dc"),
    "__ZN16IOAccelMemoryMap7prepareEv.cold.1": (0x62, "196a2d0f745818d6fb711949d6313b7b230605506ad6b315be2e04f0029953d8"),
    "__ZNK13IOAccelMemory15getPrepareCountEv": (0x48, "dadefb723a37f0c3bc231ab5518b7a388efbe4ca4fbc7a36782805fee8d12edd"),
    "__ZNK16IOAccelMemoryMap15getPrepareCountEv": (0x32, "f5f0a329bbdec86a0b989e8811cf1e5475ca993d60b985872ec159c2f83b5c95"),
    "__ZN16IOAccelSysMemory11withOptionsEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2P16IOAccelResource2jy": (0x14, "0ef5944a07cf6ee013ef23d461815067460eaf83284f843c8dd13bf2ab5ce594"),
    "__ZN16IOAccelSysMemory11withOptionsEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2P16IOAccelResource2jyb": (0x4ce, "8583979aa58b81534b6ffb0127a4d3e0a2acb7bccab413baf2ce8313a06599fb"),
    "__ZN13IOAccelMemory7prepareEv": (0x3c, "89a5c623541e69e42f71ec60268646d7d2b06fbd7ac417a9a3913ed271ae31b4"),
    "__ZN16IOAccelSysMemory20increment_wire_countEv": (0xa, "c50a571103fe420707129053dc70743bdd5d0a800993bca739f16964bbe57584"),
    "__ZN16IOAccelSysMemory20decrement_wire_countEv": (0x30, "210286dfd0c31e5992460c98a0aa3d9011967dc9897d64bbb209c79a5c66f891"),
    "__ZN16IOAccelSysMemory4wireEv": (0x34c, "b17bb9715b19099e9b766ef221adceb8f45a672016fa498f280af34dadf42e4a"),
    "__ZN22IOGraphicsAccelerator220createDMACommandPoolEv": (0x1d4, "a5b83a1786a350b2e88f404affde3ca5ce3ed042aa192f2782c9e4f7f59bffc8"),
    "__ZN22IOGraphicsAccelerator221releaseDMACommandPoolEv": (0xf0, "d24040c0993ad5617b52e19f965a7cfa37579d527d09ed8dbea943cf381aca6e"),
    "__ZN22IOGraphicsAccelerator218createIODMACommandEv": (0x40, "0920711193b7b044fc0fb1f6a5a4573c0b700d29590e8c00a458b123b450e029"),
    "__ZN22IOGraphicsAccelerator213getDMACommandEv": (0xe0, "14a72d0e573be42b52cb1566b6d1bd91d37b1ef24c267d0e590049823f8a29c7"),
    "__ZN16IOAccelSysMemory6unwireEv": (0x1f8, "0bbd5ebb7b4eb2c05d75d4fda2724cc64fe4dafa62522bd70b1ed44cf34ab8f4"),
    "__ZN16IOAccelMemoryMap11release_pteEv": (0x80, "196496fd09cb82e3766f01773b04d43e56c02beb84b067f28dc0c8af136d02db"),
    "__ZN22IOGraphicsAccelerator216returnDMACommandEP12IODMACommand": (0xa6, "27835686c339a4aec379c879beead7dfc5f89893e4518a009ee0c0f6e3315587"),
    "__ZN22IOGraphicsAccelerator214sysmem_unwiredEP16IOAccelSysMemory": (0x78, "3ba0b15e518d19f11a08d6adb1715822434e165d739eb1da4b5a7bdb638e03e3"),
    "__ZN13IOAccelMemory4freeEv": (0x80, "297d0bdaabf764760a687b384e18aea9a7a4f2c9662130814f9418937566ae61"),
    "__ZN13IOAccelMemory8completeEv": (0xa, "f159b74d1d9726151ecd33cfb99b05cdad4b4413d8fba54af1d77bed9d0fb85a"),
    "__ZN16IOAccelSysMemory4freeEv": (0x1d6, "be4bde0095f4e69781c03f296964a97f6718f0967f6e4174c182990b1885af5d"),
    "__ZN16IOAccelMemoryMap4freeEv": (0xa6, "723c9d67611abc2e6105c8dc70bd42c8b4319fb5c4f1569deec39a788702e066"),
    "__ZN13IOAccelMemory14remove_mappingEP16IOAccelMemoryMap": (0x50, "665c5afbe4c79a80c44be0ea9a032d0cee835a8d12f24720e262f722933a74a8"),
    "__ZN16IOAccelMemoryMap15remove_resourceEP16IOAccelResource2": (0x5e, "72e6f743a75a66e9d29bfa658f891e0c5c99e462a9fe3a740c44deef0eb068ea"),
    "__ZN18IOAccelDisplayPipe25get_finished_transactionsEP26DisplayTransactionListHead": (0x32, "dc7e94f2cf118d96d51d1e535455923a1375080f63d1f30c60d17afd2cad4c80"),
    "__ZN18IOAccelDisplayPipe27set_current_plane_resourcesEP12IOAccelEventjP16IOAccelResource2S3_": (0x27a, "04981414ab38c24454a83fc31237480b41cb8ec56fd2c0b55d1db74c44b1c36d"),
    "__ZN22IOGraphicsAccelerator226accel_transaction_finishedEP26DisplayTransactionListHead": (0x7e, "04858d0448d1be874ee6b0bb75d1650214e842fe5d270b0ec968e7511c6175c4"),
    "__ZN16IOAccelResource27prepareEv": (0x518, "b19dcd25b3bb38225c8eac5765a4cbc0506bb8ef5915f0d81131299626e61b2c"),
    "__ZN16IOAccelResource28completeEv": (0x74, "cf6473db8cf1433097cf4311898247f37b705e1fb0cb6a9822291d3d7c9a1c7c"),
    "__ZN18IOAccelDisplayPipe19completeTransactionEP30IOAccelDisplayPipeTransaction2": (0x152, "84b3c6c408e8018160adc80bf3b0c42d81d4cae42421f84a1d88a323f4a2bda2"),
    "__ZN30IOAccelDisplayPipeTransaction28completeEv": (0x68, "143e28ed4fb5b9169175034f013f0d151ee9922bda13d8195fa4750e19254fb9"),
    "__ZN30IOAccelDisplayPipeTransaction26finishEv": (0x64, "f4f95d1ceb27695f6b057606e408e7f2fc1e3e91add0bad78bfd12f0e67c7825"),
    "__ZN30IOAccelDisplayPipeTransaction216sendNotificationEv": (0x64, "248fd3972793ce1921e9be6de7ff8939e093e0747bff8d1cb909456e6624d139"),
    "__ZN18IOAccelDisplayPipe21isTransactionCompleteEP30IOAccelDisplayPipeTransaction2": (0x34, "0b4bd8b265303062a1ef727894d0dbe07c929113ac78b1afdd16d0f2fc41f2a6"),
    "__ZN29IOAccelDisplayPipeUserClient217sendAsyncResult64EPyiS0_j": (0xa, "d472474ff08f07583f2766d024ba52a8a15dd0d3edf602500024374c82d6e286"),
    "__ZN18IOAccelDisplayPipe23transaction_queue_gatedEP30IOAccelDisplayPipeTransaction2": (0xac, "58a2d74c983748b5f770631e50efaa309c3b8cb8d682fe0f70564acd8047e43b"),
    "__ZN30IOAccelDisplayPipeTransaction24freeEv": (0x1e2, "1864cfe99602e508dbd727f0551a2baec535805b34a95e8d99599e3ca7142c41"),
    "__ZN30IOAccelDisplayPipeTransaction27prepareEv": (0x150, "c0324bf1dd0bcf47d1c344b29b7fb09d82903820426c1192ea22c51b41cc6f7f"),
    "__ZN18IOAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2": (0x14, "e2c6e0c648573fd646ef67b2f53c9df6282278772026dc82f686f78d102ada79"),
    "__ZN18IOAccelDisplayPipe16beginTransactionEP12IOAccelEvent": (0x6, "5a96d1fb661d55552184ea24023ae8190bd1523ae1f855a8d671b07143e8b1df"),
    "__ZN29IOAccelDisplayPipeUserClient217s_transaction_endEPS_PvP25IOExternalMethodArguments": (0xe, "000d47cc2f714b2e7c26089183cd6f4294485f8bc812ca6075ec39a66a4378ff"),
    "__ZN18IOAccelDisplayPipe15transaction_endEP29IOAccelDisplayPipeUserClient2P33IOAccelDisplayPipeTransactionArgs": (0x1b4, "29932dad1b6c1449a3b10c9024f3f67a629b75c00d66bc8583abcf8ffb076851"),
    "__ZN18IOAccelDisplayPipe21displayModeWillChangeEv": (0x34, "a2a5161d9727962a15fcb4f3e1522e28a7cf8c765b284596f9bcb56bbe3f33e2"),
    "__ZN18IOAccelDisplayPipe21framebufferTerminatedEv": (0x6, "5a96d1fb661d55552184ea24023ae8190bd1523ae1f855a8d671b07143e8b1df"),
    "__ZN29IOAccelDisplayPipeUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv": (0x2e, "8e35a9e1fddb14c9f1737b913eb5b0bc129fb2213a9511eb99eb24b735a29e01"),
    "__ZN29IOAccelDisplayPipeUserClient25startEP9IOService": (0x6c, "34a3cb936cd474e5bd7a05bf82c3a50b9c96a3777974a34416ad9f2ae6bbb4be"),
    "__ZN29IOAccelDisplayPipeUserClient212setPipeIndexEjPy": (0x168, "3508def8fc565e30080f81daf666c91661831c1be3d813a4faa1e1db7f9f6186"),
    "__ZN18IOAccelDisplayPipe22user_client_terminatedEP29IOAccelDisplayPipeUserClient2": (0x7e, "15ed7924ad16dfcc1218e5ec575a32725e54478c96af47c76617a7e8aa737a35"),
    "__ZN18IOAccelDisplayPipe22framebuffer_terminatedEv": (0xc2, "8157e478b4691a37a77b01131b33fd94287471871195ae5c4f39ab21497f3e30"),
    "__ZN18IOAccelDisplayPipe27submitFlipBufferTransactionEP12IOAccelEventjP16IOAccelResource2S3_": (0x12a, "3144a7201d7237f7128c77f2c897e12920caa14c8292ecaa6b621fb572f5991c"),
    "__ZN29IOAccelDisplayPipeUserClient24stopEP9IOService": (0x12c, "1860c591fd835a8f4041dc81157830c8f649411340c23420302091b923438d2e"),
    "__ZN29IOAccelDisplayPipeUserClient213requestNotifyEPyP35IOAccelDisplayPipeRequestNotifyArgs": (0xde, "f459216034eb6d3b960fe64dc7ef6af8f7774cf8a5cd1e1825c11f676d586aa6"),
    "__ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs": (0x232, "a04f9eb8f67fb1ac6479ebaea99a3bb3658e976367a5d44dbe2ee0d7b85f275e"),
    "__ZN18IOAccelDisplayPipe4initEP22IOGraphicsAccelerator2P21IOAccelDisplayMachineP13IOFramebufferj": (0x32c, "757d5afc16b89e8d8baa3866f93be1fb37a8c81ba1266cb2a1a581330e65a48d"),
    "__ZN18IOAccelDisplayPipe26wait_for_queue_slot_nolockEv": (0x48, "5a942dec43525fd589bd15c359dda7012773fc1577e450a5c797af87a4b32479"),
    "__ZN18IOAccelDisplayPipe17transaction_queueEP30IOAccelDisplayPipeTransaction2": (0x4a, "84c686f29480a66d4baed253b1bcf7c6cbf1a4bffc692f0af612b4b8348df2fa"),
    "__ZN18IOAccelDisplayPipe14request_notifyEPyP40IOAccelDisplayPipeRequestNotifyGatedArgs": (0x58, "b7c2b65c2088ef1aa8faccbe63c59fa4be6706a853343713b06e927f163c6873"),
    "__ZN18IOAccelDisplayPipe13remove_notifyEP31IOAccelDisplayPipeNotifyRequest": (0x4a, "49b41bc3f8a99cc0bf97a457a95aca1b3f10457c543a25d14efd6f91cea31d0f"),
    "__ZN21IOAccelDisplayMachine17get_pipe_workloopEv": (0x2a, "157f3d97ec2fe80f182305de154332520cc32972b904bc171d892887d2432f45"),
    "__ZN18IOAccelDisplayPipe30release_live_transaction_gatedEv": (0x40, "59fc652bb7111c94f78994d096c32d8975b89efcffc899fb84bf164eb9c22930"),
    "__ZN18IOAccelDisplayPipe14createWorkLoopEv": (0x5e, "ec85a9941105b2c0609e4d88c429af33ea225e7aa68004df43de5dd7dbf841ab"),
    "__ZN18IOAccelDisplayPipe14setup_workloopEv": (0x2a0, "560d22438aedc391485ab3cc0a600e0bbe688ccf51af278cac7f0963a81f664c"),
    "__ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv": (0x80, "b56c911f2bd45b54ed57ec3a0a4c953ecb276b73294cdc7010a6d2d4613ea5b2"),
    "__ZN18IOAccelDisplayPipe22finishTransactionQueueEv": (0x80, "90e48c306b49108c13fe51b498211bc595515b3646fc2246190f1aa9d5f75eb4"),
    "__ZN18IOAccelDisplayPipe22releaseLiveTransactionEv": (0xf2, "a74c9fca08374a803dd5164a319ad0c8316495eddf05947e3fb1b9a678757f33"),
    "__ZN18IOAccelDisplayPipe17teardown_workloopEv": (0x102, "7eaef7309212aeb6e8b24f5a33fd1afd2e6717f921ebdc10bdb717ea0d1bacaa"),
    "__ZN18IOAccelDisplayPipe28transaction_queue_idle_gatedEv": (0xc0, "3f53a333ca96043b151d13f7e74e357bc32f6928ff86da507b9cbf4d40be846b"),
    "__ZN18IOAccelDisplayPipe31get_finished_transactions_gatedEP26DisplayTransactionListHead": (0x48, "807c04039ace117e7d22fd3895f8d2818b0b81c9fbeca903f6a3c76f66c58cd2"),
    "__ZN18IOAccelDisplayPipe23teardown_workloop_gatedEv": (0x132, "5d8affc81c42f9cb7313aea453525fd850e5df1a03e1fc9f9ec8d02845b0ccdc"),
    "__ZN18IOAccelDisplayPipe21event_interrupt_gatedEv": (0x41e, "82edb6e16a07280b596ab1f53104bebf5a8944f416833483099f4d16085d17d5"),
    "__ZN18IOAccelDisplayPipe17device_terminatedEv": (0x12, "44f2d112215eea94d1bdb399f2086b870445bf148e5553b81d163c3aa9b68105"),
    "__ZN18IOAccelDisplayPipe4freeEv": (0x1c0, "1c4ecbd1a0d601f1d0afab535af0dc1a2439503840d59d6c8d38eb7dab0cfac5"),
    "__ZN18IOAccelDisplayPipe22enable_event_interruptEPK12IOAccelEvent": (0x4c, "2b2a6a9ebe4035e86d5d4351eebfba8f31e1ec2fa33bb58f55aef27dd030efcc"),
    "__ZN18IOAccelDisplayPipe23disable_event_interruptEPK12IOAccelEvent": (0x52, "7ec7631f9126589adf8d984ec137c71381b42cd1fecdb900768da2140a2091e5"),
    "__ZN24IOAccelEventMachineFast226enableEventStampInterruptsEPK12IOAccelEvent": (0x66, "93974e6cdc73ba252aed43b7c0b42d08759c0b937b014f40b74cd8fbd5dac47a"),
    "__ZN24IOAccelEventMachineFast211finishStampEi": (0x158, "32f9a4ca4dbc18d2fceb8ca7c393a91565f766baca120340529088c8ea7afa7d"),
    "__ZN24IOAccelEventMachineFast215finishAllStampsEv": (0x6c, "09a27596eebdca0fac5ad94ae61f8a33a4bd822b3e4121d84433b82c14b12a1e"),
    "__ZN24IOAccelEventMachineFast24freeEv": (0x12, "de5103312cac712958fb96393f449efae2caaf0868144344864c1f96a6b5341b"),
    "__ZN20IOAccelEventMachine24freeEv": (0x108, "171509afb4d553c5f408f236d968a8465f431ffa74c33f2c1c31b073fb545996"),
    "__ZN20IOAccelEventMachine24stopEv": (0x63, "94b5760836e67bde79f7b2246e6aa701ea75026b42e7f7af8b5b311d919e9591"),
    "____ZN20IOAccelEventMachine24stopEv_block_invoke": (0x8b, "044430d23f6ebae467b72e05ecfa9d92eeb69bb1a43422c5d87195d4d25de4db"),
    "__ZN24IOAccelEventMachineFast227disableEventStampInterruptsEPK12IOAccelEvent": (0x66, "4784b5d57c1cb8f9b7a430da8683cc7c11752ca018a108453b3408a412b9f25e"),
}
# Complete Tahoe 25G229 base/legacy surface producer bodies and the adjacent
# base-client dispatch owners.  These contracts establish the static call and
# lock inventory only; they do not prove GPU completion or stop-time drain.
BASE_CLIENT_BODIES = {
    "__ZN14IOAccelSurface26getTargetAndMethodForIndexEPP9IOServicej":
        (0x26, "accdd4f5594a23b4eb074443ddde0d1b3ba55273ec59e6e5ee32c18c05fb6924"),
    "__ZN14IOAccelSurface14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv":
        (0x34a, "4b32552c7fdea46d26306e62d056d993dd27022ff4a5cdda1206b4a1620d20e9"),
    "__ZN14IOAccelSurface11s_set_shapeEPS_PvP25IOExternalMethodArguments":
        (0xb4, "dd2ff5691cdf3a025bdc7ba336d292711314ee83dcb19eaca5e787c42cdfb08b"),
    "__ZN14IOAccelSurface21s_signal_shared_eventEPS_PvP25IOExternalMethodArguments":
        (0x18, "d913994177a5d8ddc784e3d199e6d9d3bcc96a9e49d3db968efa554000d27d6f"),
    "__ZN14IOAccelSurface25surface_read_lock_optionsEjP25IOAccelSurfaceInformationy":
        (0x1e, "ff4b6e11554ce460d44162ad83f984d735bc5ef540d1728c825e7a7e73278620"),
    "__ZN14IOAccelSurface27surface_read_unlock_optionsEj":
        (0x18, "fac5b089dd0dd7434d2374245629e9ab2cc4587114b5e20ebed4dab6fec687a3"),
    "__ZN14IOAccelSurface9get_stateEP24eIOAccelSurfaceStateBits":
        (0x28, "ca2d4b40ae1b3fa2571b805746079819c2c7609078fe01c0825c5e8013734342"),
    "__ZN14IOAccelSurface26surface_write_lock_optionsEjP25IOAccelSurfaceInformationy":
        (0x1e, "559d8d80516c401458454349dfb7eba9704981d3e22d8e55e477353866efa08c"),
    "__ZN14IOAccelSurface28surface_write_unlock_optionsEj":
        (0x18, "e497f248ed0c0c64981ddaf68945e187fb48e7faf6a0800f0d5c89bb91668a48"),
    "__ZN14IOAccelSurface12surface_readEP22IOAccelSurfaceReadDatay":
        (0x48e, "9922ce0a17045dfbfd6941bd609a30a2ca653ea913ea5750f51d93f6ed52e4b3"),
    "__ZN14IOAccelSurface9set_scaleEjP21IOAccelSurfaceScalingy":
        (0xd6, "8afd0ae57182ab6130935b686e0d60f5b036029b143ad0e0a0f89a97b779d819"),
    "__ZN14IOAccelSurface18surface_query_lockEv":
        (0x8, "5b1ee01dedea0fcb3ecce46b2207fda587d289c2e69a0afdb9d54f2595762f2e"),
    "__ZN14IOAccelSurface17surface_read_lockEP25IOAccelSurfaceInformationy":
        (0x22, "66c09ea84b068582cd085819423aa83c6f404e6a96f765be9483c346fa79e871"),
    "__ZN14IOAccelSurface19surface_read_unlockEv":
        (0x1c, "214f9e0a5577c36e3064d0b3084ea1bc073a7a4d22cecc6b04c26e1b915328de"),
    "__ZN14IOAccelSurface18surface_write_lockEP25IOAccelSurfaceInformationy":
        (0x22, "34d431809b6becdd259a452455d76a31248ae14ad2809c55a0eda9e92b0f1946"),
    "__ZN14IOAccelSurface20surface_write_unlockEv":
        (0x1c, "7c31c219e1f444cb143b780489172326d96457612d95c922f2bce7a5d6eb4cc1"),
    "__ZN14IOAccelSurface15surface_controlEjjPj":
        (0xbe, "7377adff51379674d38e32d6da57f951fcc7ce5a88d3467de94046dc2a2ac256"),
    "__ZN14IOAccelSurface22surface_unlock_optionsE9eLockTypej":
        (0x132, "61d79b93b94ed81da4a3666359b18a1385a14679ca748f2a92f1a76ebff6b410"),
    "__ZN14IOAccelSurface11set_id_modeEjj":
        (0x2d4, "350590e0c5c7733e169fea03bf7887c00dbff79d2d895a591390945b51706f42"),
    "__ZN14IOAccelSurface9set_shapeE24eIOAccelSurfaceShapeBitsjP19IOAccelDeviceRegiony":
        (0x28, "ef0c8ed53810f662bb1e15c6b82c69f7dbfa9ca33883f9446cb0dda63bdf9c54"),
    "__ZN14IOAccelSurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony":
        (0x8a0, "8aa2c1df8f6136e01ad6bdd9486ae77289bac24beaac381b12e823c25b4fa979"),
    "__ZN14IOAccelSurface11set_scalingEjP21IOAccelSurfaceScaling":
        (0x1ac, "6b40016353f1f0009ab2ea25b73295da6d582d49c826d3157a6f29ffc13805f6"),
    "__ZN14IOAccelSurface25surface_control_with_lockEjjPj":
        (0x1a8, "9f5beb9693465e2d483332d81a80852f9508c0033ee70e702c8b9f4e31d74b0e"),
    "__ZN14IOAccelSurface18update_displayableEv":
        (0x24e, "8f38ea2ee3b8b14a56450b1030861b572164e6c4016c3b5aa2877a6dcb3e3f56"),
    "__ZN14IOAccelSurface12update_shapeEv":
        (0x3ca, "d30572c1b011bc1b3b2d4d8b159dfaf68297f1cb83a498b9511c24af1beb7fdf"),
    "__ZN14IOAccelSurface12flip_buffersEv":
        (0x40, "8328a4ce113f3c19caa257c7114c6caed74d27cd757a4a3c5df51efc9bb385bd"),
    "__ZN20IOAccelLegacySurface13surface_flushEjj":
        (0x4ae, "3ecd557cb7c6f2b94368e03793262c29055284b394ef8764829a37de1a2f95e3"),
    "__ZN20IOAccelLegacySurface25present_surface_with_swapEjj":
        (0x128, "d7fdbeee1237184679c55645622df85c7ca5d63ec1c3d790c7e37feb4b71fa82"),
    "__ZN20IOAccelLegacySurface19submit_scanout_swapEjj":
        (0x180, "07eb7774e2a6d1abb08a4c7333273074e0d5025c91d8852623dbd8e916e67350"),
    "__ZN20IOAccelLegacySurface11submit_swapEjj":
        (0x544, "2b966e6b3c19ddf433b1251d78dd7ecd4b99066c6be755a3e45554aea2bb58ec"),
    "__ZN20IOAccelLegacySurface11set_id_modeEjj":
        (0x71e, "709a078e99e63c32d8ee3cb22c433b66f2070728b8d4e478bc77c569b886f31e"),
    "__ZN20IOAccelLegacySurface20surface_lock_optionsE9eLockTypejP25IOAccelSurfaceInformationy":
        (0x47a, "98e60748d676ab8d81c36ca8d78dfa96194d5cf1cdad421229cdba97fe78a5b9"),
    "__ZN20IOAccelLegacySurface9set_shapeE24eIOAccelSurfaceShapeBitsjP19IOAccelDeviceRegiony":
        (0x4e, "1ff198a8e90156dc83724b44541675974166c4ef37908acc43c4bdcd402fc31e"),
    "__ZN20IOAccelLegacySurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony":
        (0xb8c, "4645604d782312526b1266376a563af718338cb0675526cc12ee58050d77d70b"),
    "__ZN20IOAccelLegacySurface11set_scalingEjP21IOAccelSurfaceScaling":
        (0x210, "51eb600b33a9ce7647e115104bed63768af3ddcfeb6b8af60753df4594e1489d"),
    "__ZN20IOAccelLegacySurface25surface_control_with_lockEjjPj":
        (0x110, "810161a7763790bbefe9d3dd47ada0aaa63431d38713c315fa200ec69d077b7e"),
    "__ZN20IOAccelLegacySurface12update_shapeEv":
        (0x51e, "7ca6d38f2e103f127a349624273aa2a496c8aace36f7c2f87db73d277265e40c"),
    "__ZN20IOAccelLegacySurface12copy_forwardEjP19IOAccelDeviceRegiony":
        (0x2a4, "7b0ed6631a9bfbaf97908e8ec1a3dda744b9fd75587caa8cb56554cdb7a23e96"),
    "__ZN20IOAccelLegacySurface10did_updateEjP19IOAccelDeviceRegiony":
        (0x152, "e062b42422c3c8f09015ea550c54bdf39ba06ee21bf86a340c5dd7df44f8074d"),
    "__ZN20IOAccelLegacySurface22submitFullScreenUpdateEj":
        (0x60, "d0be3507d30df6a834f91e26a538c015fe02c1a3ca251b0676ced65ac6cbc6ce"),
    "__ZN20IOAccelLegacySurface13didSubmitSwapEjj":
        (0x6, "5a96d1fb661d55552184ea24023ae8190bd1523ae1f855a8d671b07143e8b1df"),
    "__ZN20IOAccelLegacySurface17isBackBufferReadyEj":
        (0x8, "aaa500a73706124bc5374dc27c8b444160b15dc8a45b0fef9354b23106b76348"),
    "__ZN20IOAccelLegacySurface17submitCopyForwardEP12IOAccelEventjP16IOAccelResource2S3_PK13IOAccelBoundsj":
        (0xc, "98bbbcb705f834dd29c76767fe0c3cb90a15bbbc235e7dfa742ff7d6f0c8625f"),
    "__ZN20IOAccelLegacySurface12submitUpdateEjP13IOAccelBoundsj":
        (0xb, "38833f1eed078fe65bc30b3d08fec9be666f429175763e34f56cb2a450898764"),
    "__ZN20IOAccelLegacySurface15pickPresentTypeEj":
        (0xa4, "a33a46e7c49e782c314a044c2fff00d9a8498eecd538cb7cd70f69323dd33d81"),
    "__ZN14IOAccelDevice214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv":
        (0x15e, "8bfd190a99fd5ff033652e885aa5e1eefab0119016fc55cf9686f70a3346dbb5"),
    "__ZN14IOAccelDevice226getTargetAndMethodForIndexEPP9IOServicej":
        (0x26, "456d7fec0a4295b4ee09409fdde8cadd03205858c5a0e17129a26ef54169be63"),
    "__ZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv":
        (0x1be, "498599782c5a3d1cb42b0f3d280aa454c8fe32dfbc00c113d86375b4b070240e"),
    "__ZN24IOAccelSharedUserClient226getTargetAndMethodForIndexEPP9IOServicej":
        (0x26, "71c379f3428b33493e28baa044b945a97a94b8729163a757aff7ac1800c29f8d"),
    "__ZN24IOAccelSharedUserClient222process_dirty_commandsEv":
        (0x9c, "c911271d23a8d373b1d7a0f732f222d0eac0dcd5192f6a670c9b20a81f7a0d62"),
    "__ZN14IOAccelShared228processResourceDirtyCommandsEv":
        (0x174, "03d68eb2483824c5316f89cca04ddad418fc81382252f8c494df7d760729624b"),
    "__ZN17IOAccelGLContext211processSwapE7eDoSwap":
        (0x454, "dbaa260c153479bcb1ad99a9ef461db0fe0c1619ea3e11655325d13d51a4fa9e"),
    "__ZN17IOAccelGLContext211read_bufferEP30IOAccelGLContextReadBufferData":
        (0x7f6, "653e970bdc05fdc755292460a2262ac42ff3bfc067755b73b7ec4508e493b013"),
    "__ZN17IOAccelGLContext211set_surfaceEP30IOAccelGLContextSetSurfaceData":
        (0x6d6, "791e8f840b79b634ff988f2c5b8ab53407ef6922ce2662aaf7b5fd530bad6432"),
    "__ZN17IOAccelGLContext212contextStartEv":
        (0x120, "0d4a7fd04c4eab47cd8d72c119cdcf84f2332ad48a140dbc6af5e2da9349f841"),
    "__ZN17IOAccelGLContext213s_read_bufferEPS_PvP25IOExternalMethodArguments":
        (0xe, "e1a0b8ea9510811af8e7f82316e2ef0ba7b3493a2261c8b85a7b918bf066a94c"),
    "__ZN17IOAccelGLContext213s_set_surfaceEPS_PvP25IOExternalMethodArguments":
        (0xe, "48b08f0fd5bbc2c84dd2a0f8b2bc8774a159552026333e2af6d07c87665b64a9"),
    "__ZN17IOAccelGLContext213set_swap_rectEiiii":
        (0x26, "e8337d689652ab825b3ddcb1ea1e0e6f71ccbeff3919e748115166cc08ea478b"),
    "__ZN17IOAccelGLContext214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv":
        (0x4c, "89f72ebe618875b2b28b08cd8eec62aff35c42c5b8cef61a99fc64abf073d553"),
    "__ZN17IOAccelGLContext215s_set_swap_rectEPS_PvP25IOExternalMethodArguments":
        (0x38, "d2ff59f12687a58fb7fb5e3ad9ca48bbcfa04a9095a7450fbeea5ddaa2401fc7"),
    "__ZN17IOAccelGLContext217set_swap_intervalEii":
        (0x16, "93fb6d7e706e361fc34b20ec57371a7d905437560ba73baab331eca0745050a9"),
    "__ZN17IOAccelGLContext218processDataBuffersEj":
        (0x6b6, "f404c83e08bbf8e91391ccece622bd80fa328c1a56c0c12f76d622ab5b246788"),
    "__ZN17IOAccelGLContext219s_set_swap_intervalEPS_PvP25IOExternalMethodArguments":
        (0x22, "7e2c9f537f3f38ce3e1e5b25b27291d4786ff0bb7c2b44e57593ce1246bc2575"),
    "__ZN17IOAccelGLContext226set_surface_volatile_stateEj":
        (0xa8, "a2f931c465481032b4acd0fc8eb31ee5eb67c747d3ec5ad69fd2421f59785aca"),
    "__ZN17IOAccelGLContext228s_set_surface_volatile_stateEPS_PvP25IOExternalMethodArguments":
        (0x14, "3371d7f487f6232be12c3049315d2f54a0f2056442abd88a0ff220e6255a2dff"),
    "__ZN17IOAccelGLContext229set_surface_get_config_statusEP30IOAccelGLContextSetSurfaceDataP31IOAccelGLContextGetConfigStatusyPy":
        (0x498, "eaa144d03b4de8b9543e4e6ff742025e3ed6e78b96f1d20dcef7583a4f224f66"),
    "__ZN17IOAccelGLContext231s_set_surface_get_config_statusEPS_PvP25IOExternalMethodArguments":
        (0x12, "8a18da9787c26db8ba536be0e1c051b058d26798465a5cd9af12c6de4b98e15b"),
    "__ZN27IOAccelGLDrawableUserClient11set_surfaceEP37IOAccelGLDrawableClientSetSurfaceData":
        (0x516, "5ed7d0946d71b23eac02248dc47c66c094d9307b2c0a22bb0311aa6e4c868cf0"),
    "__ZN27IOAccelGLDrawableUserClient13s_set_surfaceEPS_PvP25IOExternalMethodArguments":
        (0xe, "9317414bdcab3bd5521e5f07a20e7cab2d495c76fa78cb9f14052f764b266dc4"),
    "__ZN27IOAccelGLDrawableUserClient13set_swap_rectEiiii":
        (0x26, "8b5ab890824d07db161fc93f061bc3d28d611cf723390bbfa175c8cd6c938590"),
    "__ZN27IOAccelGLDrawableUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv":
        (0x4e, "17c99d89e6685f45446c8fa6a4cd57e8e20bbcfd4a90142a0a0d1506d6fa5d10"),
    "__ZN27IOAccelGLDrawableUserClient15s_set_swap_rectEPS_PvP25IOExternalMethodArguments":
        (0x38, "ef916f445150e7d4af3f3e93bbc7d031b98285c4a3c6674b2f217b5f2e7516e8"),
    "__ZN27IOAccelGLDrawableUserClient17set_swap_intervalEii":
        (0x16, "6cdd65bc620549f5da9ad6628496d5b3a7faac13fac79d7364d2e2eaa227db6e"),
    "__ZN27IOAccelGLDrawableUserClient19s_set_swap_intervalEPS_PvP25IOExternalMethodArguments":
        (0x22, "d55b2467b2112363c8930df82314ca5bc98f2e773c84d647891463861590e3de"),
    "__ZN27IOAccelGLDrawableUserClient19signal_shared_eventEP4taskyjy":
        (0x138, "7959f70b7f08397841cc681df395792a01f212a89729511ceb4c3b135acc2ccb"),
    "__ZN27IOAccelGLDrawableUserClient21s_signal_shared_eventEPS_PvP25IOExternalMethodArguments":
        (0x20, "1b3ae518d429e91d95bfcd1d29e1bddf98d01f3d8d07fd6b1b059e362b110dd7"),
    "__ZN27IOAccelGLDrawableUserClient29set_surface_get_config_statusEP37IOAccelGLDrawableClientSetSurfaceDataP38IOAccelGLDrawableClientGetConfigStatusyPy":
        (0x418, "8ad324a97ebce8de3899e8f9f76d11b4d2d17824f4bd22a734b82c59f0f475f6"),
    "__ZN27IOAccelGLDrawableUserClient31create_mach_port_from_iosurfaceEyPj":
        (0xe2, "dc9bca4042075f0f40f74afa98a616a5bf8077ce4b11f972772f0e992750cb45"),
    "__ZN27IOAccelGLDrawableUserClient31s_set_surface_get_config_statusEPS_PvP25IOExternalMethodArguments":
        (0x12, "d44aacde2fd21c5514805e6ccd78e18c290b3904788b58f31047e22c56793ab2"),
    "__ZN27IOAccelGLDrawableUserClient33s_create_mach_port_from_iosurfaceEPS_PvP25IOExternalMethodArguments":
        (0x16, "48087eb113c4fbce75bdd823ccf5449c2941a902bfa2157a751b414ed72d5668"),
    "__ZN17IOAccelSurfaceMTL11s_set_shapeEPS_PvP25IOExternalMethodArguments":
        (0xc0, "0ef1d9133810abb73d2d1ab0d682718e0898fdd974986910c603010f031c2339"),
    "__ZN17IOAccelSurfaceMTL11set_id_modeEjj":
        (0x27e, "4c327e8f963e65198931f69b920a00cf32fffc88f87798de419310bd53c7b0e7"),
    "__ZN17IOAccelSurfaceMTL11set_scalingEjP21IOAccelSurfaceScaling":
        (0x100, "238af5065a5251e7fcbe06da368c2a3a872a0c944163738a0ee60baf2a3167b8"),
    "__ZN17IOAccelSurfaceMTL11surfaceStopEv":
        (0x190, "37adc4b77f35b81fe9fc10a1bc678a7f77042fc73e35131c41921fecd9ae3e07"),
    "__ZN17IOAccelSurfaceMTL12shapeSurfaceEjtt":
        (0x88, "089c6708ff58b3bd5e5ed8c4dfb9667f72a4b6de77d01242bba2fa39c75543ac"),
    "__ZN17IOAccelSurfaceMTL12surfaceStartEv":
        (0x11c, "c0df854c78002fded275d6c9dca496ebb0df17f633ec5cee2d113202ec46dbb3"),
    "__ZN17IOAccelSurfaceMTL12surface_readEP22IOAccelSurfaceReadDatay":
        (0xc, "98bbbcb705f834dd29c76767fe0c3cb90a15bbbc235e7dfa742ff7d6f0c8625f"),
    "__ZN17IOAccelSurfaceMTL12update_shapeEv":
        (0x182, "a350d8a4f8d40aeef590fb69c4bc3b5146e636f3130afc2cea2695275982be90"),
    "__ZN17IOAccelSurfaceMTL13surface_flushEjj":
        (0xc, "98bbbcb705f834dd29c76767fe0c3cb90a15bbbc235e7dfa742ff7d6f0c8625f"),
    "__ZN17IOAccelSurfaceMTL14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv":
        (0x84, "e80fc82f4552692dadd288b68cb8930aab277218b0eb402c925f8548cf28a992"),
    "__ZN17IOAccelSurfaceMTL15surface_controlEjjPj":
        (0xba, "382e6ddddb00f7b8609e0ab14568a63aa4cfcb40667a3d84bf8c1c6a0c68b5fb"),
    "__ZN17IOAccelSurfaceMTL17surface_read_lockEP25IOAccelSurfaceInformationy":
        (0x10, "78d6cb491d8bfaf74fc02f694643563f92c7d5504560b1f5d46651eae29e2ef9"),
    "__ZN17IOAccelSurfaceMTL18surface_query_lockEv":
        (0x8, "5b1ee01dedea0fcb3ecce46b2207fda587d289c2e69a0afdb9d54f2595762f2e"),
    "__ZN17IOAccelSurfaceMTL18surface_write_lockEP25IOAccelSurfaceInformationy":
        (0x10, "ad210ac5f24090afd21376f442551bbcbfcad7cf475b194cc24ed1b9e53670c6"),
    "__ZN17IOAccelSurfaceMTL19signal_shared_eventEP4taskyjy":
        (0x25c, "c075038ebcc0532fbc331eeacbb1736325ddc988954b4a5f709531b923256627"),
    "__ZN17IOAccelSurfaceMTL19surface_read_unlockEv":
        (0x8, "5b1ee01dedea0fcb3ecce46b2207fda587d289c2e69a0afdb9d54f2595762f2e"),
    "__ZN17IOAccelSurfaceMTL20surface_write_unlockEv":
        (0x8, "5b1ee01dedea0fcb3ecce46b2207fda587d289c2e69a0afdb9d54f2595762f2e"),
    "__ZN17IOAccelSurfaceMTL21s_signal_shared_eventEPS_PvP25IOExternalMethodArguments":
        (0x20, "34ec3bc2ad33d23cd7467cc013130ce396cf5b856dc22740555f64cc107f061c"),
    "__ZN17IOAccelSurfaceMTL25surface_control_with_lockEjjPj":
        (0x132, "43912d812c0e43dff9ed0b9e6148a0dcf0cc0f2fafbb23b7c6363777e808f4c7"),
    "__ZN17IOAccelSurfaceMTL25surface_read_lock_optionsEjP25IOAccelSurfaceInformationy":
        (0x10, "dc26e1f285d3babf622b34ba8906d2e01e951871d0948ec06de7338a367d6b39"),
    "__ZN17IOAccelSurfaceMTL26getTargetAndMethodForIndexEPP9IOServicej":
        (0x26, "3d3bf9a4e23ded2723ef2fdb6cf9e76ec69cff1aacf7ac97476fa15ac9a1e222"),
    "__ZN17IOAccelSurfaceMTL26surface_write_lock_optionsEjP25IOAccelSurfaceInformationy":
        (0x10, "8e485fa4f44f9f2ca62cfadde685f22804ed0392aeea3bc2ce702bc97e9bf4f0"),
    "__ZN17IOAccelSurfaceMTL27surface_read_unlock_optionsEj":
        (0x8, "5b1ee01dedea0fcb3ecce46b2207fda587d289c2e69a0afdb9d54f2595762f2e"),
    "__ZN17IOAccelSurfaceMTL28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony":
        (0x3a0, "9d2f18034e3f7d36934cb4e8eae52b1641c1ac77e19c7a1032096a4155465911"),
    "__ZN17IOAccelSurfaceMTL28surface_write_unlock_optionsEj":
        (0x8, "5b1ee01dedea0fcb3ecce46b2207fda587d289c2e69a0afdb9d54f2595762f2e"),
    "__ZN17IOAccelSurfaceMTL9get_stateEP24eIOAccelSurfaceStateBits":
        (0x28, "ccc5dcfa7e3acf3068becbe1d0a8abeaf0f0c4017b6b04101029b0498de9da84"),
    "__ZN17IOAccelSurfaceMTL9set_scaleEjP21IOAccelSurfaceScalingy":
        (0xd2, "8a273e7b7eaa61745adc7320d385e7512394306da74ed9c3b218e9b4a1e21ed1"),
    "__ZN17IOAccelSurfaceMTL9set_shapeE24eIOAccelSurfaceShapeBitsjP19IOAccelDeviceRegiony":
        (0x26, "41926b32c523057c59d6df54ba1c090624fea5f3530aeccaece50bde1e977eec"),
    "__ZN14IOAccelDevice210get_configEP23IOAccelDeviceConfigData":
        (0x124, "05b5c4fbacf0f03bf6e23a4837b29f8a3f8dc4a184a60a46b6e693043054dded"),
    "__ZN14IOAccelDevice28get_nameEPc":
        (0x38, "adbb44b5294fc2346457b633ff70a238dbf95c84c07b7232f6f2bbe846f523ad"),
    "__ZN14IOAccelDevice217get_event_machineEP29IOAccelDeviceEventMachineData":
        (0x146, "52c3f1ebf4adb173da3dd8fb47f209af2d55aff0184819fc5cd905037ee207e8"),
    "__ZN14IOAccelDevice216get_surface_infoEjP24IOAccelDeviceSurfaceData":
        (0x1c8, "dc7998bc4aaccad56f7eb23e76c8b772096b2999f7808a6a57939cbcd9d53131"),
    "__ZN14IOAccelDevice210set_stereoEjj":
        (0xc, "98bbbcb705f834dd29c76767fe0c3cb90a15bbbc235e7dfa742ff7d6f0c8625f"),
    "__ZN14IOAccelDevice225get_next_global_object_idEP31IOAccelDeviceGlobalObjectIDData":
        (0x26, "e9a72b22860e0d7b8069aac4890232a4cd65491d9e325ecc0f8d6ab776abb7ff"),
    "__ZN14IOAccelDevice224get_current_trace_filterEP28IOAccelDeviceTraceFilterData":
        (0x10, "030108f12f12b7f409fe349f13d6358c98434da1ab66c425e195e30c62b71da0"),
    "__ZN14IOAccelDevice215get_device_infoEP27IOAccelDeviceInfoReturnData":
        (0x80, "d31f11f0fc3ec2d8df3fd13a105fe765e79676ebf5ec035ed938c7e638f37c9f"),
    "__ZN14IOAccelDevice218get_next_gid_groupEP25IOAccelDeviceGIDGroupData":
        (0x44, "9458b3fe4a2ed420f329237925a7219f93c174b8a5cb103803104dc15766f8d5"),
    "__ZN14IOAccelDevice216set_api_propertyEP24IOAccelDeviceAPIProperty":
        (0x7a, "9c1dd1e87cfb5b3e70cc9d6f4e2111a42ecc4fcea9db53a7f2b3724a45746060"),
    "__ZN24IOAccelSharedUserClient214s_new_resourceEPS_PvP25IOExternalMethodArguments":
        (0x12c, "9df17dd0e7e7b7a65e0ed735507352d8777daf1acc4c41017971e819061b0210"),
    "__ZN24IOAccelSharedUserClient225s_set_resources_purgeableEPS_PvP25IOExternalMethodArguments":
        (0xc, "98bbbcb705f834dd29c76767fe0c3cb90a15bbbc235e7dfa742ff7d6f0c8625f"),
    "__ZN24IOAccelSharedUserClient212new_resourceEP22IOAccelNewResourceArgsP28IOAccelNewResourceReturnDatayPj":
        (0xc6a, "7c4731cbbf652662eae7535d288183c02242c9545945c2030a1065bb22ca313e"),
    "__ZN24IOAccelSharedUserClient215delete_resourceEj":
        (0x116, "3284f01291ec4103e1a0695dcad54bdc3722cdb8a303c867459b7e3cce0cb302"),
    "__ZN24IOAccelSharedUserClient217page_off_resourceEP32IOAccelSharedPageoffResourceArgs":
        (0x21a, "36174de689a22cf12264f90081fe739832e22c6f03fe698a01f56927391112b4"),
    "__ZN24IOAccelSharedUserClient219finish_object_eventEjj":
        (0x202, "80134a8ab44e54f367762ee8a6e4dbdcbbbd6df4f8cd3044a6fc155b2bfce426"),
    "__ZN24IOAccelSharedUserClient222set_resource_purgeableEj25eIOAccelResourcePurgeablePS0_":
        (0x15e, "4ef42c91323db15b05cfa08986519b25363f85568c36cf3090ce05cedc46e2be"),
    "__ZN24IOAccelSharedUserClient216get_surface_infoEjPjS0_S0_S0_S0_":
        (0x1d6, "9d56deb1f80aa81a8fa306dc5afbaf739773d0ecf9de29c1d2807bf28248496a"),
    "__ZN24IOAccelSharedUserClient217get_resource_infoEjP32IOAccelGetResourceInfoReturnDataPj":
        (0x10c, "c819568ea95fa8135a9cd8309884f81fa5d632147f5b9f19a7cc857ae92f83d0"),
    "__ZN24IOAccelSharedUserClient212create_shmemEjP22IOAccelDeviceShmemData":
        (0xc0, "2cc73e4edc6ab94b5240bfee74afd7d6cb1721d74997d702b5e9da124c975dee"),
    "__ZN24IOAccelSharedUserClient213destroy_shmemEj":
        (0xa6, "1f6bf43999fdad152e5abd3de903beee9effbdb12079c2c153710754e6b349ca"),
    "__ZN24IOAccelSharedUserClient215get_shared_infoEP30IOAccelSharedGetInfoReturnData":
        (0xea, "6c3295465e9ef5be5371f5c3dab32f200abce4674789a994d2b93052dc56dcbe"),
    "__ZN24IOAccelSharedUserClient216setup_dirty_ringEP37IOAccelSharedSetupDirtyRingReturnData":
        (0xae, "f9b862dbabb2e02eed6cba14ee8651aa39804e749916e6b0d2758a4ed54eacf9"),
    "__ZN24IOAccelSharedUserClient221allocate_fence_memoryEPyS0_":
        (0xb8, "d24ac332aa7553c55ce4f299a85b5b265312489702e48970a5dc5e167ce65ad7"),
    "__ZN24IOAccelSharedUserClient215create_mtleventEPyP27IOAccelCreateMTLEventResult":
        (0xb8, "6632bd775075bfdf67007a139252ff98d9d94e5c49685db84189185f82ecd263"),
    "__ZN24IOAccelSharedUserClient216destroy_mtleventEj":
        (0xa6, "45743bcd83efd0c58e14f7c8d677db695ae659b622284c64d494661554318c39"),
    "__ZN24IOAccelSharedUserClient215get_memory_dataEP17IOAccelMemoryData":
        (0xa2, "1c603474cedc2584729db63a09f2691d8db160232d68c09e6fa27a9784ed14dc"),
    "__ZN24IOAccelSharedUserClient215disconnect_peerEj":
        (0xa6, "460afad9a726fc56b28dfafbaf89392ea6080dd9e8f716a4c3a0f76cecf6b7da"),
    "__ZN24IOAccelSharedUserClient223set_resources_purgeableEPKj25eIOAccelResourcePurgeablePS2_i":
        (0xc, "98bbbcb705f834dd29c76767fe0c3cb90a15bbbc235e7dfa742ff7d6f0c8625f"),
    "__ZN24IOAccelSharedUserClient219get_resource_offsetEPyS0_":
        (0x116, "0900038adc1771a5bd7bef461f29ba33486d3a3aec6998fb70e93d7567be0496"),
    "__ZN24IOAccelSharedUserClient218get_allocated_sizeEP20IOAccelAllocatedSize":
        (0xa6, "2b3383addbb54587781cef9a002541747dd01e20bc3e2cda2ec89c11e1d6becb"),
    "__ZN24IOAccelSharedUserClient227set_resource_owner_identityEP43IOAccelResourceSetResourceOwnerIdentityData":
        (0xb8, "84c79db9d8299c5d4ecb870f00704a8218caa5cfd976245e9abbaa9e96cf05b4"),
    "__ZN16IOAccelResource214pageonIfNeededEv":
        (0x382, "f18b73e4af2ce5556617d49fc15d36e8b588ea4497178fcdf0f8945d2d65680c"),
    "__ZN16IOAccelResource215pageoffIfNeededEjj":
        (0x756, "1587d228f067cf5e94f9fe1279850152a703e0b1707289e5ddafe642b03b69bc"),
    "__ZN16IOAccelResource29gartEventEv":
        (0x62, "2536f1ae64bb4554c5e5cee42ba2514d05cbf086166474e0dcf749a5e944293b"),
    "__ZN16IOAccelResource28completeEv":
        (0x74, "cf6473db8cf1433097cf4311898247f37b705e1fb0cb6a9822291d3d7c9a1c7c"),
    "__ZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv":
        (0x36, "da2345b3b79afedfe949e9286e987b233896b5879c3096ec738469f05be5e812"),
    "__ZN27IOAccelMemoryInfoUserClient20s_gather_memory_dataEP8OSObjectPvP25IOExternalMethodArguments":
        (0xe4, "9ee51f0ad35f01a9967d9f9a78c51dfe8e10634d9cb2f5ed0355940d9ff00566"),
    "__ZN27IOAccelMemoryInfoUserClient27s_gather_memory_data_totalsEP8OSObjectPvP25IOExternalMethodArguments":
        (0x56, "e52c98c99ad8037502fb1519f639e46b11af0e32e87013d11b0da6a32b4c5a3d"),
    "__ZN27IOAccelMemoryInfoUserClient22s_purge_all_vid_memoryEP8OSObjectPvP25IOExternalMethodArguments":
        (0x4a, "c021c79e2a49b56e4b2c48fd7eff09716fe24bbb1fd51529879e3cceece39ed9"),
    "__ZN27IOAccelMemoryInfoUserClient19s_allowed_to_gatherEv":
        (0x50, "f24ef8f4d4b1b1568e8ac85143a328e07b3e5587c3e8a52a740c46cc2fe23ad6"),
    "__ZN27IOAccelMemoryInfoUserClient18gather_memory_dataEjPjS0_Pv":
        (0x166, "ea877cd9d739fab9d5a03d1935e2f29c957fd6efafba4672dda46d5dc09ff09f"),
    "__ZN27IOAccelMemoryInfoUserClient25gather_memory_data_totalsEP12OSDictionaryPjP28IOAccelMemoryInfoAllocTotals":
        (0x8c, "270f220c145621fd89c73329866fc6facfbb19fbba36ee312d8eea4b1ea25e8f"),
    "__ZN27IOAccelMemoryInfoUserClient20purge_all_vid_memoryEv":
        (0x7a, "067d5eac57b441ee8021e546860373e03c6613d2d89081d483c56c02624a0135"),
    "__ZN27IOAccelMemoryInfoUserClient17lock_with_timeoutEy":
        (0xae, "adfcb726151be178a643992d631d2c9efcb4400747b1bace2d0134dc522904b4"),
    "__ZN29IOAccelDisplayPipeUserClient216s_set_pipe_indexEPS_PvP25IOExternalMethodArguments":
        (0x14, "8d8f4c496e71e9ea007a83d4c88cc682efa43a14fefc2ad89521c4ec84d35ff1"),
    "__ZN29IOAccelDisplayPipeUserClient225s_get_display_mode_scalerEPS_PvP25IOExternalMethodArguments":
        (0xe, "2ebc54bdc3ac35603d97592bb8a210c94af92deda37b17b47f465d7653110b8b"),
    "__ZN29IOAccelDisplayPipeUserClient223s_get_capabilities_dataEPS_PvP25IOExternalMethodArguments":
        (0xa6, "7fcd647dfe1ed5e73567fda1b7ca312cd10dbade31c717ef30130aba84c9fea9"),
    "__ZN29IOAccelDisplayPipeUserClient216s_request_notifyEPS_PvP25IOExternalMethodArguments":
        (0x24, "b0b334b2dd97005f1f53444a61a6071ade920d3e7b9f266e811f0ccfc7925b45"),
    "__ZN29IOAccelDisplayPipeUserClient219s_transaction_beginEPS_PvP25IOExternalMethodArguments":
        (0x3a, "1d9638bafd888e86d63abb6fe4cdd7a4d0755ec3818cd76563db3ea198e7b920"),
    "__ZN29IOAccelDisplayPipeUserClient235s_transaction_set_plane_gamma_tableEPS_PvP25IOExternalMethodArguments":
        (0xb4, "f73c5104313df533dd6a7bd3dbe4d3685f5569235138cc6e87c316c3965044b6"),
    "__ZN29IOAccelDisplayPipeUserClient237s_transaction_set_pipe_pregamma_tableEPS_PvP25IOExternalMethodArguments":
        (0xb4, "d7d99c0e34b799a25992a389d978670ce33f249351c58c86b04d436ab8f54577"),
    "__ZN29IOAccelDisplayPipeUserClient238s_transaction_set_pipe_postgamma_tableEPS_PvP25IOExternalMethodArguments":
        (0xb4, "3d894115d668bc93ddaf9010985b94bbab8f59253d9eaaab0f25f23874dee5df"),
    "__ZN29IOAccelDisplayPipeUserClient218s_transaction_waitEPS_PvP25IOExternalMethodArguments":
        (0xe, "8bbcb93a040864504b7e75444576d5e6e28fe44b99ca637c11ce9802b8f48c71"),
    "__ZN29IOAccelDisplayPipeUserClient246s_transaction_set_pipe_precsclinearization_vidEPS_PvP25IOExternalMethodArguments":
        (0xb0, "51a8cf658aa2bed6dfed41459c11995451fb4683346b49ccfe7d91e912296aae"),
    "__ZN29IOAccelDisplayPipeUserClient239s_transaction_set_pipe_postcscgamma_vidEPS_PvP25IOExternalMethodArguments":
        (0xb0, "0c30ffafcfe19fae32427c08492fb5ad287b5d164f85144cb9f8ae9f4af1bdc0"),
    "__ZN29IOAccelDisplayPipeUserClient214s_copy_surfaceEPS_PvP25IOExternalMethodArguments":
        (0x3a, "24cdff9f77b53b3f396f2ae87312788189d21193bb1483c2f3c4012cd1dcce96"),
    "__ZN29IOAccelDisplayPipeUserClient28s_triageEPS_PvP25IOExternalMethodArguments":
        (0xae, "45d2483195691e3f8606d38a118990c502b4ca3fcf6da1cbc3c7aa84a9c4aeed"),
    "__ZN29IOAccelDisplayPipeUserClient24initEP12OSDictionaryP4task":
        (0x64, "73373a01026baeb58dbd03bfb5d841d929dd2b0a14b5b96f8bd9a618196c0406"),
    "__ZN29IOAccelDisplayPipeUserClient24freeEv":
        (0x30, "bbf5c9d31f93bcc7b2e2907bd257098dd64da6b1802d216d43b3c1f8b2eada96"),
    "__ZN29IOAccelDisplayPipeUserClient211clientCloseEv":
        (0xe6, "3d2639a2ec3f706477e4736e575164aa8a8b3e84944394faeaacb468494cc37e"),
    "__ZN29IOAccelDisplayPipeUserClient219setAsyncReference64EPyP8ipc_portyy":
        (0xa, "b68adbb0b2e045c7742eaeca26b851ccde3a4c06f4804937215ab27e7c768aa8"),
    "__ZN29IOAccelDisplayPipeUserClient217canUseDisplayPipeEv":
        (0x12, "cf581a8d8a278e22ea3af9c3cde70d8870e55d993945be83bd58b7ac73144fe7"),
    "__ZN29IOAccelDisplayPipeUserClient220getDisplayModeScalerEP24IOAccelDisplayPipeScaler":
        (0xb8, "daae18e26bc5951a32f6e514b552beb38139d06edf08dc7e171d85303c2d0cad"),
    "__ZN29IOAccelDisplayPipeUserClient219getCapabilitiesDataEPhPy":
        (0x154, "86960bb8c39d4692f693bf0dad7cd9b9a6d6ccd248367e9133d1cda11debf785"),
    "__ZN29IOAccelDisplayPipeUserClient216transactionBeginEPj":
        (0xd0, "2dc8436bdacf4b9d8c48d149c8a913b3f746e300234b5328f8bba22f67c6d7ab"),
    "__ZN29IOAccelDisplayPipeUserClient229transactionSetPlaneGammaTableEP32IOAccelDisplayPipeGammaTableArgs":
        (0xc2, "93db83d3a31e7264b9c6d72157993892d342e5150217605b070694156a19ec95"),
    "__ZN29IOAccelDisplayPipeUserClient231transactionSetPipePreGammaTableEP32IOAccelDisplayPipeGammaTableArgs":
        (0xc2, "337320a637c36894a713ce93426a1c770d5dda59993c633753e2ee23a5957c55"),
    "__ZN29IOAccelDisplayPipeUserClient232transactionSetPipePostGammaTableEP32IOAccelDisplayPipeGammaTableArgs":
        (0xc2, "cc3718f07d8798b07293c81c52c4d4db431e8b3ae80ac615d09c25c92dda6267"),
    "__ZN29IOAccelDisplayPipeUserClient240transactionSetPipePreCSCLinearizationVIDEP41IOAccelDisplayPipePreCSCLinearizationArgs":
        (0xc2, "6bf6fd46f44ed88b3ed51ee7776761e1eb0c435ae109b49fee4323de4d9b8b0e"),
    "__ZN29IOAccelDisplayPipeUserClient233transactionSetPipePostCSCGammaVIDEP39IOAccelDisplayPipePostCSCGammaTableArgs":
        (0xc2, "3880145ec24a601439b533268ece03525ec8dfbaae91b1dc9c810c0d7b3bdceb"),
    "__ZN29IOAccelDisplayPipeUserClient215transactionWaitEP37IOAccelDisplayPipeTransactionWaitArgs":
        (0x1b6, "02a9a5264c8555d61107884827605363f07900a3d1bbd5d5866573ab3676aa60"),
    "__ZN29IOAccelDisplayPipeUserClient220getDisplayPipeNoLockEv":
        (0x3a, "485b972f9bea8760e9b7b207383940fb07b83c1966455eab117635b188cb24c6"),
    "__ZN29IOAccelDisplayPipeUserClient211copySurfaceEjj":
        (0xc6, "d87e338ab8d96d5cfcd0072ae73c6ab8c99b14650bbe143f0e9c2d3ba2208806"),
    "__ZN29IOAccelDisplayPipeUserClient220doesEntitlementExistEPKc":
        (0x5c, "24a6818ad9be5cd5f09ad2bbb40aa36723b97d510355c8913a9f4f1f469cd04c"),
    "__ZN29IOAccelDisplayPipeUserClient26triageEPPcPy":
        (0x48, "9bf247cf23439f5099ab1889c785fcde4cc4d46b94f5d577dcb1d6f05633797e"),
    "__ZN18IOAccelDisplayPipe11copySurfaceEjj":
        (0x1fe, "24d5b168385d91babdb3cc50a86e85ad5c062bc8b8081c6a7c909a5274f1f52b"),
    "__ZN22IOGraphicsAccelerator217createDisplayPipeEP13IOFramebufferj":
        (0x5e, "d9afbef0e40721dd997e0c187d36b515441d8e0ffe24effe39ccfef217927b66"),
    "__ZN21IOAccelDisplayMachine4initEP22IOGraphicsAccelerator2":
        (0x38, "d5504a921904e99e95c6aab0a050e94b1149bff5fbe1deb0c78f7974c812489c"),
    "__ZN21IOAccelDisplayMachine5startEP11IOPCIDevice":
        (0x1f8, "32c0db11361bde95208839ad3884d6ed7273e1c1821329c5a9059e7b4c3eabda"),
    "__ZNK21IOAccelDisplayMachine19getFramebufferCountEv":
        (0xc, "d415df741f33607acc471cbba18d7656dc0c5c1b82c98411942957484200bb16"),
    "__ZNK21IOAccelDisplayMachine14getDisplayPipeEj":
        (0x10, "93725164a8a6be58cd158f1598de8bfec4af2b3832d57743c0f6cd989df9cb3f"),
    "__ZN21IOAccelDisplayMachine17found_framebufferEP13IOFramebuffer":
        (0xb6, "8b94db88f1a6ae0e5c4c4ea9688c823ab4cb1bd6b653b05aad2c14ed3b719519"),
    "__ZN27IOAccelLegacyDisplayMachine5startEP11IOPCIDevice":
        (0x12, "2cc74bf2d9e3055a4214e2e74fdcde1970fd26c3b090e813791404b818189660"),
    "__ZN27IOAccelLegacyDisplayMachine17found_framebufferEP13IOFramebuffer":
        (0x7a, "c172a83062fc19d7f945fc4652c7b97a7d5c430f26e73854a50a2dd8ed12229e"),
}

# Complete bodies on the inherited resource paging bridge and the independent
# control paths that can reach it.  These hashes are deliberately separate
# from BASE_CLIENT_BODIES: they establish the page-on/page-off call inventory,
# but do not classify every caller as new work versus stop-time retirement.
RESOURCE_PAGING_BODIES = {
    "__ZN16IOAccelResource24loadEv":
        (0x5e, "30fdcd2d6c973ff3a78f94df1995a79539820e83e57b342d5bf11a03fde2f31b"),
    "__ZN16IOAccelResource26unloadEv":
        (0x5e, "5b58c54a08b7e68d718836eab29d1daf776118ed10c71cee00ae21586b4a9ed1"),
    "__ZN16IOAccelResource27unpurgeEb":
        (0x152, "8e50b07ce04ef6d06d3e5e1a96f00190f075c1298bb068dd4b7ad8671327c673"),
    "__ZN16IOAccelResource215pageoffInLinearEv":
        (0x11a, "7784a3457241d083690fe143104b095eb20831096bb09506ed99eb467078789b"),
    "__ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi":
        (0x5b8, "20eefd146a8f1d32929e277627dc8c8aae3a5ec85701eb1678b8eb66d17ac0d1"),
    "__ZN16IOAccelResource217getPhysicalOffsetEyPy":
        (0x70, "f2f2f1c356f8d8a8de0cd0c75b59efcbea10fbbcdf2233ba72c4403713e973de"),
    "__ZN14IOAccelSurface15pageoffInLinearEv":
        (0x4e, "da8b58f019547eea3ca0528039658934c66ca74a166249035d9641bf5e3b3fb7"),
    "__ZN22IOGraphicsAccelerator222pageoffSurfaceInLinearEv":
        (0x44, "5045e9eae20a1a27397bd03dea65fcf9cb962cfc8d0f00d1acfa9aaaa97556b4"),
    "__ZN22IOGraphicsAccelerator218deviceCacheControlEP20IOSurfaceDeviceCachejyy":
        (0x4c8, "dd5856e492fa174d14f87f77493353012cf61bd94265adc75f3ab51cd1e52451"),
    "__ZN22IOGraphicsAccelerator220emitFirstFlushEventsEv":
        (0x1d8, "a6f8b156e93108b0a2bd0670c83ac12ea5e07e30bee4caeee1055d140b3c1810"),
    "__ZN22IOGraphicsAccelerator226try_unload_dirty_resourcesEj":
        (0x12c, "0b7e275093d7d7d12892f70230210018d26a35338a7439b788a9400fdb84e3d3"),
    "__ZN22IOGraphicsAccelerator222unload_dirty_resourcesEv":
        (0x16c, "2e792839b0339b767b09b0f1652cbb83a0958f10b48fd0cfb222de49d5335e45"),
    "__ZN22IOGraphicsAccelerator218unwireAllVidMemoryEv":
        (0xa0, "33fc9c9f60ad926dce70c3769759f5399d028c2569934527afaeb59b13363a24"),
    "__ZL23IOAcceleratorKDCallbackPv16kd_callback_typeS_":
        (0x261, "396c2f7256646008b50e587dd9ee035f8d2df251c50ecac1e133b8541520e482"),
    "__ZN21IOAccelDisplayMachine24display_mode_will_changeEj":
        (0x1d0, "626449a853fb2283ff02dd43ed689c0a5d3399de667566adf4fccb4a4aa76fcc"),
    "__ZN27IOAccelLegacyDisplayMachine24display_mode_will_changeEj":
        (0x2c2, "66a4b2ba61ce0d9c20193e2ae37fd2c5ea9e75b79c854fd2f3be748b5bf3b644"),
    "__ZN24IOAccelLegacyDisplayPipe26framebuffer_will_power_offEv":
        (0x36, "0c3d2f56e3af5dc08436bbc9070bd9ec98e3fbc401be7ad84cdb30b363e3f15c"),
    "__ZN18IOAccelDisplayPipe20displayModeDidChangeEv":
        (0x1a4, "d49ec6f742ecd2539309dead66526513d515f0934d74b0d4efd4b0350a566ebe"),
    "__ZN24IOAccelLegacyDisplayPipe21displayModeWillChangeEv":
        (0x7e, "331586042fa2c5606e036a55b91a93132b0c4ab1418a5e1187c98a8a1a55dd67"),
    "__ZN24IOAccelLegacyDisplayPipe20displayModeDidChangeEv":
        (0x254, "191f39b3628ccbd9f7b697d7bb993084aebb92d9a0318ad2ba8b75afe08409ae"),
    "__ZN18IOAccelDisplayPipe18wsaaWillEnterDeferEi":
        (0x7c, "a55161b0a536bd1167017060b322749316ddb27324812bda4bfb84d17e7d7e7e"),
    "__ZN20IOAccelLegacySurface20update_exclusive_bitEjbb":
        (0x2b2, "6aecc25e82a8da01904fc0bc94b5e01852b26a52b8d9c09f2400547f154b8999"),
    "__ZN14IOAccelSurface20surface_lock_optionsE9eLockTypejP25IOAccelSurfaceInformationy":
        (0x43c, "596c382406f1dcf1472364b03272b11fc2356378a1e96defb6fc5437d3108433"),
    "__ZN27IOAccelMemoryInfoUserClient5startEP9IOService":
        (0x50, "70319ef24efe543bb227447962c8f2926abdbf22bb34dd3ecf56701df3674f12"),
    "__ZN18IOAccelDisplayPipe22display_change_handlerEPvP13IOFramebufferiS0_":
        (0x410, "62fe06fa92852756fb95efd54847ba84a740171e090bee460801dbcc8c0e3e00"),
    "__ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej":
        (0x134, "7f3ffd5740c0e649d8f41f614bbdf22ebf53edcea160ae3550bad5d9adde88d0"),
    "__ZN22IOGraphicsAccelerator212oneTimeSetupEv":
        (0x106, "90dbeebed8a7d20215a270c26ead5b003c79eceb08a8cb0343797a7ed1e27e62"),
    "__ZN22IOGraphicsAccelerator219acceleratorFinalizeEv":
        (0x30, "ccb417e4534182a8a4302902a733a57b0267faaa521b94d5aaf284d12ab05901"),
    "__ZN21IOAccelDisplayMachine23display_mode_did_changeEj":
        (0x1e4, "62a146e74693332913f38776f14872331df492c755ec7b1eaa7c5de98281b358"),
    "__ZN21IOAccelDisplayMachine26framebuffer_will_power_offEj":
        (0xa6, "6838dce61bcb8c594a47b288226201a66ac112744a1ac1128643e9e8ce285953"),
    "__ZN21IOAccelDisplayMachine24framebuffer_did_power_onEj":
        (0xa6, "982891589cee8d926ff49aaf106292a9a6f8524d556e6f01dfa892d66cd46d90"),
    "__ZN21IOAccelDisplayMachine21wsaa_will_enter_deferEji":
        (0xc8, "f89459ee63f9f67d92c060286befb001edc1175d157f9c1f87f74031e1920662"),
    "__ZN21IOAccelDisplayMachine20wsaa_will_exit_deferEji":
        (0xc8, "1886024d7f5d78161147688fbc36c0e6029da5d7210cf9b108f809dfcbbf3494"),
    "__ZN21IOAccelDisplayMachine17system_will_sleepEv":
        (0x36, "7aa7eb970dacfff23e471569593b3765d03fb5a919d5459ff859c829063b7296"),
    "__ZN27IOAccelLegacyDisplayMachine23display_mode_did_changeEj":
        (0x220, "688b51d9ef974c7754a50a7ee1c6ed0f4c1d54ff117e3afcf7d802bf07ca2504"),
    "__ZN27IOAccelLegacyDisplayMachine26framebuffer_will_power_offEj":
        (0x120, "c0c91fd8c4d83f4454161f7eee4b5cd1eb96c821a0e5557c40ba313b31352c89"),
    "__ZN27IOAccelLegacyDisplayMachine24framebuffer_did_power_onEj":
        (0x11a, "070e912a4c01032a7f17d0492536482d02ecf0ddb2ef82f9b10fddc67c34569b"),
    "__ZN24IOAccelLegacyDisplayPipe20save_scanout_surfaceEv":
        (0x38, "3908a7ddc5c22e8aa767dfc921a8dfda750ca5155d4dd7971dcef1cddd897952"),
    "__ZN24IOAccelLegacyDisplayPipe23save_fullscreen_surfaceEv":
        (0x1d4, "83c4d32b195342d6c1981d7393973505af79ca42d0f45d65e6ae0413270ae0c9"),
    "__ZN24IOAccelLegacyDisplayPipe24framebuffer_did_power_onEv":
        (0x5a, "999be24e952c9d1538b6654714fd9974b10825c3caadc1e4d0355d966cbc2a31"),
    "__ZN24IOAccelLegacyDisplayPipe23restore_scanout_surfaceEv":
        (0x30, "0bfa4c73bba0d21c81f630b59b6e78cc769e0b5a4d3d9669479a9293d080fd85"),
    "__ZN24IOAccelLegacyDisplayPipe26restore_fullscreen_surfaceEv":
        (0x60, "904439242aa5ead6512413863c63660d0fee9f026dab8969571d82e09824be9d"),
    "__ZN18IOAccelDisplayPipe17wsaaWillExitDeferEi":
        (0x11a, "38f19835b1fcd300b27d24b29729987e3fab6df6f974479212673736e07332a3"),
    "__ZN18IOAccelDisplayPipe26framebuffer_will_power_offEv":
        (0x3a, "e4634b01d2bacce413b19917267f65dfd12341800f1406e0fc2e2fd3e949e729"),
    "__ZN18IOAccelDisplayPipe24framebuffer_did_power_onEv":
        (0x18, "db558f4554d6c9cda5446f4b2e4c250083932b67b931a196470c32f7320e1489"),
}

# IOGraphicsAccelerator2 defers IOSurface cache retirement through a dedicated
# workloop source before stop removes that source.  Keep this lifecycle
# separate from ordinary paging roots: it decides whether selector 3/4
# callbacks are teardown work rather than rejectable post-close producers.
ACCELERATOR_FINALIZE_BODIES = {
    "__ZN22IOGraphicsAccelerator218finalize_interruptEP22IOInterruptEventSourcei":
        (0x116, "abbfe51fc8d1667ac056185dfeb2186ecc1c97580a00de3a96a9186b36dc84af"),
    "__ZN22IOGraphicsAccelerator28finalizeEj":
        (0x72, "7815960d1c0bab062cfeb62230ec05eb36da6d88564fda072e28b0aeabd58b94"),
    "__ZN22IOGraphicsAccelerator220finalize_if_possibleEv":
        (0x3e, "1d3f17c11b29c3544e3572704fe5375c9222b85d84acc27a8a0e5fed976e04bc"),
}

# Every x86-64 indirect call [vtable + slot] in the executable segment.  The
# set intentionally includes calls on unrelated classes that reuse the same
# numeric slot: keeping the complete executable inventory prevents a newly
# introduced resource call from being silently omitted by name heuristics.
RESOURCE_SLOT_CALL_SITES = {
    0x170: {
        0x14b6826d, 0x14b68508, 0x14b6a05c, 0x14b6e377, 0x14b6ebbd,
        0x14b6fc85, 0x14b6fd5e, 0x14b76d3c, 0x14b772e4, 0x14b773b0,
        0x14b7c13e, 0x14b7c156, 0x14b7c30b, 0x14b7c323, 0x14b7c49a,
        0x14b7c4b2, 0x14b7c64e, 0x14b7c67e, 0x14b7d087, 0x14b7d09c,
        0x14b8c8d6, 0x14b9722a, 0x14b97877, 0x14b97942, 0x14b97ab2,
        0x14b97c0f, 0x14b97e80, 0x14b9801f, 0x14b98260, 0x14b985d8,
        0x14b98682, 0x14b98c50, 0x14b9b159, 0x14b9c64b, 0x14b9c660,
        0x14b9d5cb, 0x14b9d5df, 0x14ba8113, 0x14baf60b, 0x14baf953,
        0x14baffd1, 0x14bb2bf0, 0x14bb2c22, 0x14bb3390, 0x14bb77fc,
        0x14bbc58c,
    },
    0x180: {
        0x14b83a53, 0x14b83a78, 0x14b83c2e, 0x14b83c4e, 0x14b8c082,
        0x14b959b5, 0x14bb130b, 0x14bb1490, 0x14bb1960, 0x14bb28c8,
        0x14bb785e,
    },
    0x188: {
        0x14b6764a, 0x14b697d0, 0x14b6f289, 0x14b6f584, 0x14b7b2b4,
        0x14b89ae4, 0x14b8a738, 0x14b8b63a, 0x14b8b6b2, 0x14b9127c,
        0x14b9aca1, 0x14b9be1a, 0x14b9d82b, 0x14ba26ca, 0x14ba2753,
        0x14ba407b, 0x14ba568b, 0x14bafa91, 0x14bb29fb, 0x14bb7852,
        0x14bba48b, 0x14bbadbe,
    },
    0x260: {
        0x14b89a0c, 0x14b8adfb, 0x14b8b1ed, 0x14b8b5a3, 0x14b916db,
        0x14ba3e42, 0x14ba4057, 0x14bb2310,
    },
    0x268: {
        0x14b77193, 0x14b89761, 0x14b8b197, 0x14b8b448, 0x14b92fb7,
        0x14b92ff0, 0x14b93029, 0x14b93062, 0x14b9309b, 0x14b930d4,
        0x14b9f6e2, 0x14ba0e2f, 0x14ba1afc, 0x14ba7d88, 0x14ba7dee,
        0x14ba7e64, 0x14bae559,
    },
}

# Partition every numeric-slot call site above.  The first class is reached
# only below an already inventoried external selector or one of the four
# asynchronous control roots pinned below.  Retirement calls must remain
# callable after admission closes.  Shared bridge calls can be reached from
# both classes, which is why no low-level prepare/load/unload/page-on/page-off
# route is a valid producer gate.  The final class is an unrelated receiver
# that happens to reuse the same numeric vtable offset.
RESOURCE_SLOT_CALL_SITE_CLASSES = {
    "admitted_or_control_descendant": {
        0x14b6826d, 0x14b68508, 0x14b6e377, 0x14b6ebbd,
        0x14b6fc85, 0x14b6fd5e, 0x14b76d3c, 0x14b7c13e,
        0x14b7c156, 0x14b7c30b, 0x14b7c323, 0x14b7c49a,
        0x14b7c4b2, 0x14b7c64e, 0x14b7c67e, 0x14b7d087,
        0x14b7d09c, 0x14b8c8d6, 0x14b9722a, 0x14b97877,
        0x14b97942, 0x14b97ab2, 0x14b97c0f, 0x14b97e80,
        0x14b9801f, 0x14b98260, 0x14b985d8, 0x14b98682,
        0x14b98c50, 0x14b9b159, 0x14b9c64b, 0x14b9c660,
        0x14b9d5cb, 0x14b9d5df, 0x14ba8113, 0x14baf60b,
        0x14baf953, 0x14baffd1, 0x14bb2bf0, 0x14bb2c22,
        0x14bb3390,
        0x14b6f289, 0x14b6f584, 0x14b7b2b4, 0x14b89ae4,
        0x14b8b63a, 0x14b8b6b2, 0x14b9127c, 0x14b9aca1,
        0x14b9be1a, 0x14b9d82b, 0x14ba407b, 0x14bafa91,
        0x14bba48b,
        0x14b916db, 0x14ba3e42, 0x14ba4057,
    },
    "retirement_or_teardown": {
        0x14b6764a, 0x14b697d0, 0x14ba568b, 0x14bb29fb,
        0x14bbadbe,
    },
    "shared_low_level_bridge": {
        0x14b8c082,
        0x14b8a738, 0x14ba26ca, 0x14ba2753,
        0x14b89a0c, 0x14b8adfb, 0x14b8b1ed, 0x14b8b5a3,
        0x14b89761, 0x14b8b197, 0x14b8b448,
    },
    "unrelated_receiver": {
        0x14b6a05c, 0x14b772e4, 0x14b773b0, 0x14bb77fc,
        0x14bbc58c,
        0x14b83a53, 0x14b83a78, 0x14b83c2e, 0x14b83c4e,
        0x14b959b5, 0x14bb130b, 0x14bb1490, 0x14bb1960,
        0x14bb28c8, 0x14bb785e,
        0x14bb7852,
        0x14bb2310,
        0x14b77193, 0x14b92fb7, 0x14b92ff0, 0x14b93029,
        0x14b93062, 0x14b9309b, 0x14b930d4, 0x14b9f6e2,
        0x14ba0e2f, 0x14ba1afc, 0x14ba7d88, 0x14ba7dee,
        0x14ba7e64, 0x14bae559,
    },
}
SURFACE_METHODS = (
    ("__ZN14IOAccelSurface25surface_read_lock_optionsEjP25IOAccelSurfaceInformationy", (0, 2, 1, 0xffffffff)),
    ("__ZN14IOAccelSurface27surface_read_unlock_optionsEj", (0, 0, 1, 0)),
    ("__ZN14IOAccelSurface9get_stateEP24eIOAccelSurfaceStateBits", (0, 0, 0, 1)),
    ("__ZN14IOAccelSurface26surface_write_lock_optionsEjP25IOAccelSurfaceInformationy", (0, 2, 1, 0xffffffff)),
    ("__ZN14IOAccelSurface28surface_write_unlock_optionsEj", (0, 0, 1, 0)),
    ("__ZN14IOAccelSurface12surface_readEP22IOAccelSurfaceReadDatay", (0, 4, 0, 0xffffffff)),
    (None, (0, 4, 4, 0xffffffff)),
    (None, (0, 0, 2, 0)),
    ("__ZN14IOAccelSurface9set_scaleEjP21IOAccelSurfaceScalingy", (0, 4, 1, 0xffffffff)),
    (None, (0, 4, 2, 0xffffffff)),
    (None, (0, 0, 2, 0)),
    ("__ZN14IOAccelSurface18surface_query_lockEv", (0, 0, 0, 0)),
    ("__ZN14IOAccelSurface17surface_read_lockEP25IOAccelSurfaceInformationy", (0, 2, 0, 0xffffffff)),
    ("__ZN14IOAccelSurface19surface_read_unlockEv", (0, 0, 0, 0)),
    ("__ZN14IOAccelSurface18surface_write_lockEP25IOAccelSurfaceInformationy", (0, 2, 0, 0xffffffff)),
    ("__ZN14IOAccelSurface20surface_write_unlockEv", (0, 0, 0, 0)),
    ("__ZN14IOAccelSurface15surface_controlEjjPj", (0, 0, 2, 1)),
    (None, (0, 4, 5, 0xffffffff)),
    (None, (0, 0, 0, 0)),
)
DEVICE_METHODS = (
    ("__ZN14IOAccelDevice210get_configEP23IOAccelDeviceConfigData", (0, 2, 0, 64)),
    ("__ZN14IOAccelDevice28get_nameEPc", (0, 2, 0, 64)),
    ("__ZN14IOAccelDevice217get_event_machineEP29IOAccelDeviceEventMachineData", (0, 2, 0, 600)),
    ("__ZN14IOAccelDevice216get_surface_infoEjP24IOAccelDeviceSurfaceData", (0, 2, 1, 24)),
    ("__ZN14IOAccelDevice210set_stereoEjj", (0, 0, 2, 0)),
    ("__ZN14IOAccelDevice225get_next_global_object_idEP31IOAccelDeviceGlobalObjectIDData", (0, 2, 0, 8)),
    ("__ZN14IOAccelDevice224get_current_trace_filterEP28IOAccelDeviceTraceFilterData", (0, 2, 0, 8)),
    ("__ZN14IOAccelDevice215get_device_infoEP27IOAccelDeviceInfoReturnData", (0, 2, 0, 24)),
    ("__ZN14IOAccelDevice218get_next_gid_groupEP25IOAccelDeviceGIDGroupData", (0, 2, 0, 16)),
    ("__ZN14IOAccelDevice216set_api_propertyEP24IOAccelDeviceAPIProperty", (0, 3, 16, 0xffffffff)),
)
SHARED_METHODS = (
    ("__ZN24IOAccelSharedUserClient212new_resourceEP22IOAccelNewResourceArgsP28IOAccelNewResourceReturnDatayPj", (0, 3, 0xffffffff, 0xffffffff)),
    ("__ZN24IOAccelSharedUserClient215delete_resourceEj", (0, 0, 1, 0)),
    ("__ZN24IOAccelSharedUserClient217page_off_resourceEP32IOAccelSharedPageoffResourceArgs", (0, 4, 0, 8)),
    ("__ZN24IOAccelSharedUserClient219finish_object_eventEjj", (0, 0, 2, 0)),
    ("__ZN24IOAccelSharedUserClient222set_resource_purgeableEj25eIOAccelResourcePurgeablePS0_", (0, 0, 2, 1)),
    ("__ZN24IOAccelSharedUserClient216get_surface_infoEjPjS0_S0_S0_S0_", (0, 0, 1, 5)),
    ("__ZN24IOAccelSharedUserClient217get_resource_infoEjP32IOAccelGetResourceInfoReturnDataPj", (0, 2, 1, 0xffffffff)),
    ("__ZN24IOAccelSharedUserClient212create_shmemEjP22IOAccelDeviceShmemData", (0, 2, 1, 16)),
    ("__ZN24IOAccelSharedUserClient213destroy_shmemEj", (0, 0, 1, 0)),
    ("__ZN24IOAccelSharedUserClient215get_shared_infoEP30IOAccelSharedGetInfoReturnData", (0, 2, 0, 16)),
    ("__ZN24IOAccelSharedUserClient216setup_dirty_ringEP37IOAccelSharedSetupDirtyRingReturnData", (0, 2, 0, 24)),
    ("__ZN24IOAccelSharedUserClient222process_dirty_commandsEv", (0, 0, 0, 0)),
    ("__ZN24IOAccelSharedUserClient221allocate_fence_memoryEPyS0_", (0, 3, 8, 8)),
    ("__ZN24IOAccelSharedUserClient215create_mtleventEPyP27IOAccelCreateMTLEventResult", (0, 3, 8, 24)),
    ("__ZN24IOAccelSharedUserClient216destroy_mtleventEj", (0, 0, 1, 0)),
    ("__ZN24IOAccelSharedUserClient215get_memory_dataEP17IOAccelMemoryData", (0, 2, 0, 48)),
    ("__ZN24IOAccelSharedUserClient215disconnect_peerEj", (0, 0, 1, 0)),
    ("__ZN24IOAccelSharedUserClient223set_resources_purgeableEPKj25eIOAccelResourcePurgeablePS2_i", (0, 3, 0xffffffff, 0xffffffff)),
    ("__ZN24IOAccelSharedUserClient219get_resource_offsetEPyS0_", (0, 3, 16, 8)),
    ("__ZN24IOAccelSharedUserClient218get_allocated_sizeEP20IOAccelAllocatedSize", (0, 2, 0, 8)),
    ("__ZN24IOAccelSharedUserClient227set_resource_owner_identityEP43IOAccelResourceSetResourceOwnerIdentityData", (0, 3, 16, 0)),
)
GL_CONTEXT_METHODS = (
    ("__ZN17IOAccelGLContext213s_set_surfaceEPS_PvP25IOExternalMethodArguments", (0, 0x30, 0, 0)),
    ("__ZN17IOAccelGLContext231s_set_surface_get_config_statusEPS_PvP25IOExternalMethodArguments", (0, 0x30, 0, 0x28)),
    ("__ZN17IOAccelGLContext215s_set_swap_rectEPS_PvP25IOExternalMethodArguments", (4, 0, 0, 0)),
    ("__ZN17IOAccelGLContext219s_set_swap_intervalEPS_PvP25IOExternalMethodArguments", (2, 0, 0, 0)),
    ("__ZN17IOAccelGLContext228s_set_surface_volatile_stateEPS_PvP25IOExternalMethodArguments", (1, 0, 0, 0)),
    ("__ZN17IOAccelGLContext213s_read_bufferEPS_PvP25IOExternalMethodArguments", (0, 0x20, 0, 0)),
)
GL_DRAWABLE_METHODS = (
    ("__ZN27IOAccelGLDrawableUserClient13s_set_surfaceEPS_PvP25IOExternalMethodArguments", (0, 0x30, 0, 0)),
    ("__ZN27IOAccelGLDrawableUserClient31s_set_surface_get_config_statusEPS_PvP25IOExternalMethodArguments", (0, 0x30, 0, 0x28)),
    ("__ZN27IOAccelGLDrawableUserClient15s_set_swap_rectEPS_PvP25IOExternalMethodArguments", (4, 0, 0, 0)),
    ("__ZN27IOAccelGLDrawableUserClient19s_set_swap_intervalEPS_PvP25IOExternalMethodArguments", (2, 0, 0, 0)),
    ("__ZN27IOAccelGLDrawableUserClient21s_signal_shared_eventEPS_PvP25IOExternalMethodArguments", (3, 0, 0, 0)),
    ("__ZN27IOAccelGLDrawableUserClient33s_create_mach_port_from_iosurfaceEPS_PvP25IOExternalMethodArguments", (1, 0, 1, 0)),
)
SURFACE_MTL_METHODS = (
    ("__ZN17IOAccelSurfaceMTL25surface_read_lock_optionsEjP25IOAccelSurfaceInformationy", (0, 2, 1, 0xffffffff)),
    ("__ZN17IOAccelSurfaceMTL27surface_read_unlock_optionsEj", (0, 0, 1, 0)),
    ("__ZN17IOAccelSurfaceMTL9get_stateEP24eIOAccelSurfaceStateBits", (0, 0, 0, 1)),
    ("__ZN17IOAccelSurfaceMTL26surface_write_lock_optionsEjP25IOAccelSurfaceInformationy", (0, 2, 1, 0xffffffff)),
    ("__ZN17IOAccelSurfaceMTL28surface_write_unlock_optionsEj", (0, 0, 1, 0)),
    ("__ZN17IOAccelSurfaceMTL12surface_readEP22IOAccelSurfaceReadDatay", (0, 4, 0, 0xffffffff)),
    (None, (0, 4, 4, 0xffffffff)),
    ("__ZN17IOAccelSurfaceMTL11set_id_modeEjj", (0, 0, 2, 0)),
    ("__ZN17IOAccelSurfaceMTL9set_scaleEjP21IOAccelSurfaceScalingy", (0, 4, 1, 0xffffffff)),
    ("__ZN17IOAccelSurfaceMTL9set_shapeE24eIOAccelSurfaceShapeBitsjP19IOAccelDeviceRegiony", (0, 4, 2, 0xffffffff)),
    ("__ZN17IOAccelSurfaceMTL13surface_flushEjj", (0, 0, 2, 0)),
    ("__ZN17IOAccelSurfaceMTL18surface_query_lockEv", (0, 0, 0, 0)),
    ("__ZN17IOAccelSurfaceMTL17surface_read_lockEP25IOAccelSurfaceInformationy", (0, 2, 0, 0xffffffff)),
    ("__ZN17IOAccelSurfaceMTL19surface_read_unlockEv", (0, 0, 0, 0)),
    ("__ZN17IOAccelSurfaceMTL18surface_write_lockEP25IOAccelSurfaceInformationy", (0, 2, 0, 0xffffffff)),
    ("__ZN17IOAccelSurfaceMTL20surface_write_unlockEv", (0, 0, 0, 0)),
    ("__ZN17IOAccelSurfaceMTL15surface_controlEjjPj", (0, 0, 2, 1)),
    (None, (0, 4, 5, 0xffffffff)),
    (None, (0, 0, 0, 1)),
)
MEMORY_INFO_METHODS = (
    ("__ZN27IOAccelMemoryInfoUserClient20s_gather_memory_dataEP8OSObjectPvP25IOExternalMethodArguments",
     (0, 0, 1, 0xffffffff)),
    ("__ZN27IOAccelMemoryInfoUserClient27s_gather_memory_data_totalsEP8OSObjectPvP25IOExternalMethodArguments",
     (0, 0xffffffff, 0, 0x70)),
    ("__ZN27IOAccelMemoryInfoUserClient22s_purge_all_vid_memoryEP8OSObjectPvP25IOExternalMethodArguments",
     (0, 0, 0, 0)),
)
DISPLAY_PIPE_METHODS = (
    ("__ZN29IOAccelDisplayPipeUserClient216s_set_pipe_indexEPS_PvP25IOExternalMethodArguments", (1, 0, 1, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient225s_get_display_mode_scalerEPS_PvP25IOExternalMethodArguments", (0, 0, 0, 0x18)),
    ("__ZN29IOAccelDisplayPipeUserClient223s_get_capabilities_dataEPS_PvP25IOExternalMethodArguments", (0, 0, 0, 0xffffffff)),
    ("__ZN29IOAccelDisplayPipeUserClient216s_request_notifyEPS_PvP25IOExternalMethodArguments", (0, 0x18, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient219s_transaction_beginEPS_PvP25IOExternalMethodArguments", (0, 0, 1, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient235s_transaction_set_plane_gamma_tableEPS_PvP25IOExternalMethodArguments", (0, 0xffffffff, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient237s_transaction_set_pipe_pregamma_tableEPS_PvP25IOExternalMethodArguments", (0, 0xffffffff, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient238s_transaction_set_pipe_postgamma_tableEPS_PvP25IOExternalMethodArguments", (0, 0xffffffff, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient217s_transaction_endEPS_PvP25IOExternalMethodArguments", (0, 0x118, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient218s_transaction_waitEPS_PvP25IOExternalMethodArguments", (0, 0xc, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient246s_transaction_set_pipe_precsclinearization_vidEPS_PvP25IOExternalMethodArguments", (0, 0xffffffff, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient239s_transaction_set_pipe_postcscgamma_vidEPS_PvP25IOExternalMethodArguments", (0, 0xffffffff, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient214s_copy_surfaceEPS_PvP25IOExternalMethodArguments", (2, 0, 0, 0)),
    ("__ZN29IOAccelDisplayPipeUserClient28s_triageEPS_PvP25IOExternalMethodArguments", (0, 0, 0, 0xffffffff)),
)
# Symbol-bounded bodies reviewed locally. These identities do not certify
# overridden resource methods, iterator locking, DMA completion or host safety.
SCRUB_BODIES = {
    SHARED_SCRUB: (0x54, "391e92190ff359bd4c4bede93538b83d0fc7f49677456f504528bc82ba331e00"),
    RESOURCE_SCRUB: (0xc2, "4f0a89f94134348f87d80dc77e6e66c800a85e4c50d659c6303701f653cc700d"),
    "__ZN13IOAccelMemory14scrubAllEventsEv":
        (0x5a, "43bc8b5f00af74bdc67cbad08ff4994fd24ca075fe0d2c05a45a7604c71aa492"),
    EVENT_SCRUB: (0x7e, "7b3a41f9948c644a06cb48c6e86f7a014e28b9f05aee296f5c813efc3c822833"),
}
LOCK_COPIES = {
    "__ZN22IOGraphicsAccelerator215acceleratorLockEv": {
        0x14b6c7a6: "fed7918d5caa2b65a7400e742fdad37cea80fcd1aaba8a648840d44da1f48c1e",
        0x14b7e0ba: "4e9a4289539e7a9c6b462cc5a19efc669fba9d7d397db0a8726f34456d1e4253",
        0x14b901b0: "453eaae8d172876d5d7df2346e0225d3671589c04b521f4319390f4996baf4d0",
        0x14b9975a: "901b711b0117df4a425bf80c4b2b72213d1695fd27a79f5687a0b70aecaa18c3",
    },
    "__ZN22IOGraphicsAccelerator217acceleratorUnlockEv": {
        0x14b6c7f8: "c5a432f48d0f3208f2525bd995dfc1a5fbd1239af11163b853e6d38983777a2d",
        0x14b70806: "b4f0dbfafce3dc7e3f14d8f56f98f82b54037474c765f71832c8937008395072",
        0x14b7e084: "bdbe6331672d1f8760649b884188f8e06163016b0e1a7bc4bc6bb3bf5a49ce15",
        0x14b90202: "3b470039180b89751871154bec962b1b4a7e508555ce8a3de7ea6c2493fc3bf6",
        0x14b997ac: "ba3edbb0b0b596697ac6b352f469864fd98ca922d99b0bc67a169ab587255977",
    },
}
CONTRACTS = {
    EVENT_ENABLE_STAMP: bytes.fromhex("55 48 89 e5 5d c3"),
    EVENT_DISABLE_STAMP: bytes.fromhex("55 48 89 e5 5d c3"),
    "__ZNK22IOGraphicsAccelerator223isLockedByCurrentThreadEv":
        bytes.fromhex("55 48 89 e5 b0 01 5d c3"),
    "__ZN22IOGraphicsAccelerator29lock_busyEv":
        bytes.fromhex("55 48 89 e5 48 81 c7 58 01 00 00 5d e9 7f 93 46 eb 90"),
    "__ZN22IOGraphicsAccelerator211unlock_busyEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb 48 81 c7 58 01 00 00 e8 63 93 46 eb "
                      "83 f8 01 75 39 f6 83 78 0c 00 00 04 75 30 48 8b 93 60 01 00 00 "
                      "48 85 d2 74 24 48 8b 03 48 8b 8b 68 01 00 00 48 8b 80 38 07 00 00 "
                      "48 89 df be 01 80 ff e3 45 31 c0 48 83 c4 08 5b 5d ff e0 "
                      "48 83 c4 08 5b 5d c3"),
    "__ZN22IOGraphicsAccelerator218acceleratorDidLockEPKci":
        bytes.fromhex("83 3d 11 f5 03 00 00 74 21 55 48 89 e5 e8 92 90 46 eb "
                      "bf 19 00 12 85 48 89 c6 31 d2 31 c9 45 31 c0 45 31 c9 "
                      "5d e9 a4 90 46 eb c3 90"),
    "__ZN22IOGraphicsAccelerator221acceleratorWillUnlockEPKci":
        bytes.fromhex("83 3d e5 f4 03 00 00 74 21 55 48 89 e5 e8 66 90 46 eb "
                      "bf 1a 00 12 85 48 89 c6 31 d2 31 c9 45 31 c0 45 31 c9 "
                      "5d e9 78 90 46 eb c3 90"),
    "__ZN17IOAccelSharedList8IteratorC1ERS_":
        bytes.fromhex("55 48 89 e5 48 8b 06 48 89 07 5d c3"),
    "__ZN17IOAccelSharedList8Iterator13getNextSharedEv":
        bytes.fromhex("48 8b 07 48 85 c0 74 0c 55 48 89 e5 48 8b 48 10 48 89 0f 5d c3 90"),
    "__ZN19IOAccelResourceList15ReverseIteratorC1ERS_":
        bytes.fromhex("55 48 89 e5 48 8b 46 08 48 89 07 5d c3 90"),
    "__ZN19IOAccelResourceList15ReverseIterator15getPrevResourceEv":
        bytes.fromhex("48 8b 07 48 85 c0 74 0c 55 48 89 e5 48 8b 48 50 48 89 0f 5d c3 90"),
    "__ZN22IOGraphicsAccelerator211scrubEventsEv":
        bytes.fromhex("55 48 89 e5 41 57 41 56 53 48 83 ec 28 48 89 fb "
                      "49 bf aa aa aa aa aa aa aa aa 4c 8d 75 e0 4d 89 3e "
                      "48 8d b7 88 0a 00 00 4c 89 f7 e8 b2 96 fd ff "
                      "4c 89 f7 e8 b6 96 fd ff 48 85 c0 74 0e 48 8b 08 "
                      "48 89 c7 ff 91 28 01 00 00 eb e5 4c 8d 75 c8 4d 89 3e "
                      "4d 89 7e 08 4d 89 7e 10 48 81 c3 00 0b 00 00 4c 89 f7 "
                      "48 89 de e8 84 9c fd ff 4c 89 f7 e8 8a 9c fd ff "
                      "48 85 c0 74 1d 48 8d 5d c8 48 8b 08 48 89 c7 "
                      "ff 91 28 02 00 00 48 89 df e8 6d 9c fd ff 48 85 c0 "
                      "75 e7 48 83 c4 28 5b 41 5e 41 5f 5d c3 90"),
    "__ZN15IOAccelChannel213setEventStampEP12IOAccelEvent":
        bytes.fromhex("55 48 89 e5 48 89 f2 48 8b 47 18 48 8b 80 80 03 00 00 "
                      "8b 77 20 48 8b 08 48 8b 89 d0 01 00 00 48 89 c7 5d ff e1 90"),
    "__ZN15IOAccelChannel214incrementStampEv":
        bytes.fromhex("55 48 89 e5 48 8b 47 18 48 8b 80 80 03 00 00 8b 77 20 "
                      "48 8b 08 48 8b 89 d8 01 00 00 48 89 c7 5d ff e1"),
    "__ZN15IOAccelChannel219mergeEventExcludingEP12IOAccelEventS1_":
        bytes.fromhex("55 48 89 e5 48 8b 47 18 48 8b 80 80 03 00 00 8b 4f 20 "
                      "48 8b 38 4c 8b 87 c8 01 00 00 48 89 c7 5d 41 ff e0 90"),
    EVENT_INCREMENT:
        bytes.fromhex("55 48 89 e5 41 57 41 56 53 50 48 89 fb 48 63 c6 48 c1 e0 03 "
                      "4c 8d 34 40 42 8b 84 37 fc 00 00 00 44 8d 78 01 44 31 f8 "
                      "3d 00 00 00 40 72 0d 48 8b 7b 10 48 8b 07 ff 90 f0 08 00 00 "
                      "46 89 bc 33 fc 00 00 00 48 8b 43 10 ff 80 a0 00 00 00 "
                      "48 83 c4 08 5b 41 5e 41 5f 5d c3"),
    EVENT_WRITE_STAMP:
        bytes.fromhex("55 48 89 e5 48 63 f6 48 8d 04 76 8b 84 c7 fc 00 00 00 "
                      "48 8b 17 4c 8b 82 a0 02 00 00 48 89 ca 89 c1 5d 41 ff e0 90"),
    EVENT_TERMINATE:
        bytes.fromhex("55 48 89 e5 48 8b 47 28 48 85 c0 74 30 8b 4f 30 85 c9 "
                      "7e 29 48 8d 97 04 01 00 00 31 f6 4c 8b 04 f0 4d 85 c0 "
                      "74 08 8b 0a 41 89 08 8b 4f 30 48 ff c6 4c 63 c1 48 83 "
                      "c2 18 4c 39 c6 7c e0 0f ae f8 48 8d 05 65 00 04 00 "
                      "5d ff a0 60 02 00 00"),
    "__ZN22IOGraphicsAccelerator224deviceTerminatedUnlockedEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb 48 81 c7 c8 0d 00 00 "
                      "e8 e9 dc 46 eb 85 c0 75 09 83 bb d0 0d 00 00 00 "
                      "74 07 48 83 c4 08 5b 5d c3"),
    "__ZN22IOGraphicsAccelerator217enableAcceleratorEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb f6 87 92 0c 00 00 08 75 0c "
                      "48 8b bb 80 03 00 00 e8 d8 10 fd ff 80 8b 78 0c 00 00 02 "
                      "48 83 c4 08 5b 5d c3"),
    "__ZN22IOGraphicsAccelerator218disableAcceleratorEv":
        bytes.fromhex("55 48 89 e5 53 50 48 89 fb f6 87 92 0c 00 00 08 75 0c "
                      "48 8b bb 80 03 00 00 e8 86 10 fd ff 80 a3 78 0c 00 00 fd "
                      "48 83 c4 08 5b 5d c3"),
    "__ZN20IOAccelEventMachine233stopHardwareProgressTimerUnlockedEv":
        bytes.fromhex("55 48 89 e5 f6 47 74 01 74 1a 48 8b 47 60 48 85 c0 74 11 "
                      "c6 47 74 00 48 8b 08 48 89 c7 5d ff a1 18 02 00 00 5d c3"),
    "__ZN20IOAccelEventMachine234startHardwareProgressTimerUnlockedEv":
        bytes.fromhex("55 48 89 e5 f6 47 74 01 75 20 48 8b 47 60 48 85 c0 74 17 "
                      "c6 47 74 01 8b 77 70 48 8b 08 48 8b 89 d0 01 00 00 "
                      "48 89 c7 5d ff e1 5d c3"),
    "__ZN16IOAccelMemoryMap20getGPUVirtualAddressEv":
        bytes.fromhex("f6 47 10 40 75 08 48 8b 87 98 00 00 00 c3"),
    "__ZN16IOAccelMemoryMap8completeEv":
        bytes.fromhex("55 48 89 e5 ff 4f 0c 5d c3"),
    "__ZN16IOAccelMemoryMap11finishEventEv":
        bytes.fromhex("55 48 89 e5 48 8b 87 88 00 00 00 48 8b 80 80 03 00 00 "
                      "48 8d 77 38 48 8b 08 48 8b 89 88 01 00 00 48 89 c7 5d ff e1"),
}


def commands(image, base):
    header = struct.unpack_from("<8I", image, base)
    assert header[0] == 0xFEEDFACF, "not a little-endian 64-bit Mach-O"
    offset = base + 32
    limit = offset + header[5]
    for _ in range(header[4]):
        command, size = struct.unpack_from("<II", image, offset)
        assert size >= 8 and offset + size <= limit <= len(image), "bad load command"
        yield command, offset
        offset += size
    assert offset == limit, "load-command size mismatch"


def check_boot_atomic(system, path):
    boot = pathlib.Path(path).read_bytes()
    assert hashlib.sha256(boot).hexdigest() == BOOT_SHA256, "unreviewed BootKC identity"
    segments = []
    kernel = []
    iosurface = []
    bases = []
    for command, offset in commands(boot, 0):
        if command == 0x19:
            f = struct.unpack_from("<II16sQQQQIIII", boot, offset)
            segments.append((f[3], f[5], f[6]))
            if f[2].rstrip(b"\0") == b"__HIB":
                bases.append(f[3])
        elif command == 0x80000035:
            _, _, _, file_offset, name_offset, _ = struct.unpack_from("<IIQQII", boot, offset)
            start = offset + name_offset
            identifier = boot[start:boot.index(0, start)]
            if identifier == b"com.apple.kernel":
                kernel.append(file_offset)
            elif identifier == b"com.apple.iokit.IOSurface":
                iosurface.append(file_offset)
    assert len(kernel) == len(iosurface) == len(bases) == 1, \
        "missing/ambiguous kernel/IOSurface/base"
    symbols = {name: [] for name in (b"_OSIncrementAtomic", b"_OSDecrementAtomic", b"_thread_wakeup_prim",
                                   b"__ZN15IORegistryEntry18getRegistryEntryIDEv", b"_kernel_debug",
                                   b"_kernel_debug_register_callback",
                                   b"_IOLockLock", b"_IOLockUnlock", b"_assert_wait_deadline",
                                   b"_thread_block", b"_clock_interval_to_deadline",
                                   b"__ZN10IOWorkLoop8workLoopEv",
                                   b"__ZN10IOWorkLoop14runActionBlockEU13block_pointerFivE",
                                   b"__ZN9IOService15serviceMatchingEPKcP12OSDictionary",
                                   b"__ZN9IOService19getMatchingServicesEP12OSDictionary",
                                   b"__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass",
                                   b"__ZTV22IOInterruptEventSource", b"__ZTV18IOTimerEventSource",
                                   b"__ZN22IOInterruptEventSource23normalInterruptOccurredEPvP9IOServicei",
                                   b"__ZN18IOTimerEventSource12setTimeoutUSEj",
                                   b"__ZN18IOTimerEventSource13cancelTimeoutEv",
                                   b"_thread_call_cancel", b"_thread_call_cancel_wait",
                                   b"__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E",
                                   b"__ZN18IOTimerEventSource4initEjP8OSObjectPFvS1_PS_E",
                                   b"__ZN18IOTimerEventSource4initEP8OSObjectPFvS1_PS_E",
                                   b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE",
                                   b"__ZN18IOTimerEventSource14setTimeoutFuncEv")}
    symbols[b"__ZN18IOTimerEventSource11setWorkLoopEP10IOWorkLoop"] = []
    symbols[b"__ZTV8OSObject"] = []
    for name in (b"__ZNK8OSObject6retainEv", b"__ZNK8OSObject14getRetainCountEv",
                 b"__ZNK8OSObject12taggedRetainEPKv", b"__ZNK8OSObject7releaseEv",
                 b"__ZNK8OSObject13taggedReleaseEPKv", b"__ZNK8OSObject13taggedReleaseEPKvi"):
        symbols[name] = []
    for name in (b"__ZTV12IODMACommand", b"__ZTV25IOGeneralMemoryDescriptor",
                 b"__ZN12IODMACommand12cloneCommandEPv",
                 b"__ZN12IODMACommand14initWithRefConEPv",
                 b"__ZN12IODMACommand4freeEv",
                 b"__ZN12IODMACommand7prepareEyybb",
                 b"__ZN12IODMACommand21initWithSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapperS2_",
                 b"__ZN12IODMACommand16setSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapper",
                 b"_kalloc_type_impl", b"_IOMallocTypeImpl", b"_IOFreeTypeImpl", b"__ZN8IOMapper19waitForSystemMapperEv",
                 b"_lck_mtx_alloc_init",
                 b"__ZN12IODMACommand17withSpecificationEPFbPS_NS_9Segment64EPvjEhyNS_14MappingOptionsEyjP8IOMapperS2_",
                 b"__ZN12IODMACommand19setMemoryDescriptorEPK18IOMemoryDescriptorb",
                 b"__ZNK25IOGeneralMemoryDescriptor19dmaCommandOperationEjPvj",
                 b"__ZN12IODMACommand7walkAllEj", b"_upl_abort_range",
                 b"_upl_commit_range", b"_upl_deallocate", b"_vm_page_free_list",
                 b"__ZN12IODMACommand21clearMemoryDescriptorEb",
                 b"__ZN12IODMACommand8completeEbb",
                 b"__ZN25IOGeneralMemoryDescriptor8completeEj"):
        symbols[name] = []
    symbols[b"__ZTV12IOUserClient"] = []
    symbols[b"__ZN12IOUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv"] = []
    symbols[b"__ZN8OSObject4freeEv"] = []
    symbols[b"__ZN8OSObjectdlEPvm"] = []
    symbols[b"__ZNK13IOEventSource11getWorkLoopEv"] = []
    symbols[b"__ZN13IOCommandGate11commandGateEP8OSObjectPFiS1_PvS2_S2_S2_E"] = []
    symbols[b"__ZN18IOTimerEventSource7disableEv"] = []
    symbols[b"__ZN18IOTimerEventSource10wakeAtTimeEjyy"] = []
    symbols[b"__ZN18IOTimerEventSource17timeoutAndReleaseEPvS0_"] = []
    for name in (b"__ZTV10IOWorkLoop", b"__ZN10IOWorkLoop8openGateEv",
                 b"__ZN10IOWorkLoop4initEv",
                 b"__ZTV13IOCommandGate", b"__ZN13IOCommandGate10runCommandEPvS0_S0_S0_",
                 b"__ZN13IOCommandGate4initEP8OSObjectPFiS1_PvS2_S2_S2_E",
                 b"__ZN13IOEventSource9setActionEPFvP8OSObjectzE",
                 b"__ZNK13IOCommandGate9MetaClass5allocEv",
                 b"__ZN13IOCommandGate10gMetaClassE", b"__ZTVN13IOCommandGate9MetaClassE",
                 b"__ZN13IOCommandGate9runActionEPFiP8OSObjectPvS2_S2_S2_ES2_S2_S2_S2_",
                 b"__ZN10IOWorkLoop13_maintRequestEPvS0_S0_S0_",
                 b"__ZN10IOWorkLoop9closeGateEv",
                 b"__ZN10IOWorkLoop17removeEventSourceEP13IOEventSource"):
        symbols[name] = []
    for name in (b"_IOSimpleLockFree", b"_lck_spin_free", b"_lck_spin_destroy",
                 b"__ZN8OSObject4initEv", b"__ZN8OSObjectD2Ev",
                 b"__ZNK11OSMetaClass18instanceDestructedEv",
                 b"__ZN8OSObjectC2EPK11OSMetaClass",
                 b"__ZNK11OSMetaClass19instanceConstructedEv",
                 b"__ZN8OSObjectnwEm", b"_kalloc_ext"):
        symbols[name] = []
    kernel_defined_addresses = set()
    for command, offset in commands(boot, kernel[0]):
        if command != 2:
            continue
        symbol_offset, count, string_offset, _ = struct.unpack_from("<6I", boot, offset)[2:]
        for index in range(count):
            name_offset, _, _, _, address = struct.unpack_from("<IBBHQ", boot, symbol_offset + 16 * index)
            if address:
                kernel_defined_addresses.add(address)
            start = string_offset + name_offset
            name = boot[start:boot.index(0, start)]
            if name in symbols:
                symbols[name].append(address)
    assert all(len(values) == 1 for values in symbols.values()), "missing/ambiguous kernel imports"
    def kernel_read(address, length):
        matches = [f + address - v for v, f, size in segments
                   if v <= address and address + length <= v + size]
        assert len(matches) == 1, "unmapped/ambiguous event-source implementation"
        return boot[matches[0]:matches[0] + length]

    # IOSurfaceDeviceCache owns the callback pointer installed by
    # IOGraphicsAccelerator2.  Parse that fileset directly so selector 3/4
    # retirement cannot be inferred from private names or a disassembly
    # sample.  These are the complete Tahoe 25G229 methods that retain the
    # accelerator, dispatch every selector, detach caches and terminate all
    # caches for a particular accelerator.
    surface_names = (
        b"__ZNK20IOSurfaceDeviceCache7releaseEv",
        b"__ZN20IOSurfaceDeviceCache4freeEv",
        b"__ZN20IOSurfaceDeviceCache7pageOffEv",
        b"__ZN20IOSurfaceDeviceCache4waitEv",
        b"__ZN20IOSurfaceDeviceCache4waitEb",
        b"__ZN20IOSurfaceDeviceCache12accelReleaseEv",
        b"__ZN20IOSurfaceDeviceCache5purgeEv",
        b"__ZN20IOSurfaceDeviceCache7unpurgeEb",
        b"__ZN20IOSurfaceDeviceCache13testpurgeableEv",
        b"__ZN20IOSurfaceDeviceCache12setPurgeableEj",
        b"__ZN20IOSurfaceDeviceCache20signalEventOperationEyP20IOSurfaceSharedEventy",
        b"__ZN20IOSurfaceDeviceCache14setControlFuncEPvPFvS0_PS_jyyE",
        b"__ZN9IOSurface41gatherOrphanedDeviceCachesWithAcceleratorEPvP24IOSurfaceDeviceCacheList",
        b"__ZN13IOSurfaceRoot36terminateDeviceCachesWithAcceleratorEPv",
    )
    surface_symbols = {name: [] for name in surface_names}
    surface_segments = []
    for command, offset in commands(boot, iosurface[0]):
        if command == 0x19:
            fields = struct.unpack_from("<II16sQQQQIIII", boot, offset)
            surface_segments.append((fields[3], fields[5], fields[6]))
        elif command == 2:
            symbol_offset, count, string_offset, string_size = \
                struct.unpack_from("<6I", boot, offset)[2:]
            for index in range(count):
                name_offset, _, _, _, address = struct.unpack_from(
                    "<IBBHQ", boot, symbol_offset + 16 * index)
                assert name_offset < string_size, "invalid IOSurface symbol string"
                start = string_offset + name_offset
                name = boot[start:boot.index(0, start, string_offset + string_size)]
                if name in surface_symbols:
                    surface_symbols[name].append(address)
    assert all(len(values) == 1 for values in surface_symbols.values()), \
        "missing/ambiguous IOSurface device-cache symbol"

    def surface_address(name):
        return surface_symbols[name][0]

    def surface_file_offset(address, length=1):
        matches = [file_offset + address - virtual
                   for virtual, file_offset, size in surface_segments
                   if virtual <= address and address + length <= virtual + size]
        assert len(matches) == 1, "unmapped/ambiguous IOSurface implementation"
        return matches[0]

    def surface_read(address, length):
        offset = surface_file_offset(address, length)
        return boot[offset:offset + length]

    cache_bodies = {
        b"__ZNK20IOSurfaceDeviceCache7releaseEv":
            (0x90, "4f89e1f10a03467da5f9cd88d9f5aed49ac42dc8ea0a569e1a9c120d75547ee8"),
        b"__ZN20IOSurfaceDeviceCache4freeEv":
            (0x6a, "1853f390cc65000ab614eba5c2a25e19800d2d8f23b71c642b34984c72ce61ee"),
        b"__ZN20IOSurfaceDeviceCache7pageOffEv":
            (0x2c, "e22f1f511740d68cc79ac44c52ac71decdd7cf717d42b611f61fb1eeee6f5740"),
        b"__ZN20IOSurfaceDeviceCache4waitEv":
            (0x30, "c4c4443edffe6541ebeeb0f34ac9dfa22e3c39e09522a2a73f555ffc9101086f"),
        b"__ZN20IOSurfaceDeviceCache4waitEb":
            (0x36, "cda5d6a13e15fcdfda68e66cb08e956a7cdb02bdd3b40eb72f5d6cb8029d6dce"),
        b"__ZN20IOSurfaceDeviceCache12accelReleaseEv":
            (0x30, "49638b7088b1951922b3a43d4c21062ad53f465a176644e1d6ce1cf29a8dabd6"),
        b"__ZN20IOSurfaceDeviceCache5purgeEv":
            (0x30, "85c88dc741301df2b0b710c74155e0b38b9b204378399b0e8cb7a5676fb917ea"),
        b"__ZN20IOSurfaceDeviceCache7unpurgeEb":
            (0x4c, "776bc4e5396a0890e29c86186dd68f08db44a5510ef631e2832d9d680bd18512"),
        b"__ZN20IOSurfaceDeviceCache13testpurgeableEv":
            (0x46, "568347d8c2089f648828e64f90ace8864b518c60f4af029b6b2c4f6ec723228a"),
        b"__ZN20IOSurfaceDeviceCache12setPurgeableEj":
            (0x34, "4bb2aad3ad72bc4ff1a470990439f29972d9bc6037a7d32800a9846c449c5289"),
        b"__ZN20IOSurfaceDeviceCache20signalEventOperationEyP20IOSurfaceSharedEventy":
            (0x64, "9d40903b93ce2066e0322720b54bb2e6f0acf8936c7d13a4ebdb9b5e48da7d0c"),
        b"__ZN20IOSurfaceDeviceCache14setControlFuncEPvPFvS0_PS_jyyE":
            (0xc, "5cabd619c4aeaf3b87f308c9d401659855483a27b9a4eec76a95767b3ecd3294"),
        b"__ZN9IOSurface41gatherOrphanedDeviceCachesWithAcceleratorEPvP24IOSurfaceDeviceCacheList":
            (0x114, "ab3ef8481d1f1c6326168e49af6b48d57a34bf3ae3d1ed86ba2e16ee58771584"),
        b"__ZN13IOSurfaceRoot36terminateDeviceCachesWithAcceleratorEPv":
            (0x12a, "6f1686fb3e3086559faf34aca7e633d6489c3e8d11a185260a67139f99f0ccca"),
    }
    for name, (length, digest) in cache_bodies.items():
        assert hashlib.sha256(surface_read(surface_address(name), length)).hexdigest() == digest, \
            f"changed IOSurface device-cache lifecycle: {name.decode()}"
    set_control = surface_address(
        b"__ZN20IOSurfaceDeviceCache14setControlFuncEPvPFvS0_PS_jyyE")
    assert surface_read(set_control + 4, 8) == bytes.fromhex(
        "48 89 77 20 48 89 57 28"), \
        "changed IOSurface cache resource/control callback ownership"
    cache_free = surface_address(b"__ZN20IOSurfaceDeviceCache4freeEv")
    assert surface_read(cache_free + 9, 0x2f) == bytes.fromhex(
        "48 83 7f 10 00 74 44 48 8b 7b 18 48 85 ff 74 1f "
        "48 8b 43 28 48 85 c0 74 16 48 83 7b 20 00 74 0f "
        "48 89 de ba 04 00 00 00 31 c9 45 31 c0 ff d0"), \
        "changed IOSurface cache-free selector-4 retirement callback"
    selector_methods = (
        (b"__ZN20IOSurfaceDeviceCache7pageOffEv", 0),
        (b"__ZN20IOSurfaceDeviceCache4waitEv", 2),
        (b"__ZN20IOSurfaceDeviceCache4waitEb", 2),
        (b"__ZN20IOSurfaceDeviceCache12accelReleaseEv", 3),
        (b"__ZN20IOSurfaceDeviceCache5purgeEv", 5),
        (b"__ZN20IOSurfaceDeviceCache7unpurgeEb", 6),
        (b"__ZN20IOSurfaceDeviceCache13testpurgeableEv", 8),
        (b"__ZN20IOSurfaceDeviceCache12setPurgeableEj", 7),
        (b"__ZN20IOSurfaceDeviceCache20signalEventOperationEyP20IOSurfaceSharedEventy", 9),
    )
    for name, selector in selector_methods:
        body = surface_read(surface_address(name), cache_bodies[name][0])
        selector_load = bytes((0xba, selector, 0, 0, 0)) if selector else bytes.fromhex("31 d2")
        assert body.count(selector_load) == 1, \
            f"changed IOSurface callback selector: {name.decode()}"
    terminate = surface_address(
        b"__ZN13IOSurfaceRoot36terminateDeviceCachesWithAcceleratorEPv")
    gather = surface_address(
        b"__ZN9IOSurface41gatherOrphanedDeviceCachesWithAcceleratorEPvP24IOSurfaceDeviceCacheList")
    accel_release = surface_address(b"__ZN20IOSurfaceDeviceCache12accelReleaseEv")
    assert surface_read(gather + 0x5b, 4) == bytes.fromhex("4d 39 65 18") and \
        surface_read(gather + 0x65, 5) == bytes.fromhex("41 f6 45 59 01") and \
        surface_read(gather + 0x73, 6) == bytes.fromhex("ff 50 18 83 f8 01"), \
        "changed IOSurface orphan-cache accelerator/flag/last-reference admission"
    for call_offset, target in ((0x51, gather), (0xf0, accel_release)):
        encoded = surface_read(terminate + call_offset, 5)
        assert encoded[0] == 0xe8 and terminate + call_offset + 5 + \
            struct.unpack_from("<i", encoded, 1)[0] == target, \
            "changed IOSurface cache termination gather/release edge"
    terminate_stub = 0x111ca
    assert system[terminate_stub:terminate_stub + 6] == bytes.fromhex(
        "ff 25 e8 45 01 00"), \
        "changed SystemKC IOSurface cache-termination import stub"
    terminate_pointer = terminate_stub + 6 + struct.unpack_from(
        "<i", system, terminate_stub + 2)[0]
    terminate_raw = struct.unpack_from("<Q", system, terminate_pointer)[0]
    assert terminate_raw >> 63 == 0 and (terminate_raw >> 30) & 3 == 0 and \
        terminate_raw & 0x3fffffff == surface_file_offset(terminate), \
        "SystemKC finalize import no longer resolves to IOSurface cache termination"
    print("PASS paired-KC IOSurface selectors and accelerator-cache finalize/retirement lifecycle")

    # The global KD callback never caches an accelerator pointer.  It builds a
    # fresh matching-services iterator for each notification and invokes the
    # accelerator only while that iterator retains its backing OSSet.  Pin the
    # three imported APIs; the SystemKC side below pins release ordering and
    # the exact getNextObject/dynamic-cast/callback loop.
    for stub, name in (
            (0x10282, b"__ZN9IOService15serviceMatchingEPKcP12OSDictionary"),
            (0x10288, b"__ZN9IOService19getMatchingServicesEP12OSDictionary"),
            (0x1007e, b"__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass"),
            (0x11272, b"_kernel_debug_register_callback")):
        assert system[stub:stub + 2] == b"\xff\x25", \
            "changed KD registry-iterator import stub"
        pointer = stub + 6 + struct.unpack_from("<i", system, stub + 2)[0]
        raw = struct.unpack_from("<Q", system, pointer)[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 0 and \
            bases[0] + (raw & 0x3fffffff) == symbols[name][0], \
            "changed KD registry-iterator import identity"
    print("PASS paired-KC KD matching-services iterator import identities")

    typed_new = kernel_read(0xffffff8000a1cc40, 0x50)
    typed_alloc = kernel_read(0xffffff8000369980, 0x90)
    assert hashlib.sha256(typed_alloc).hexdigest() == "5f34a636c2f1fbc91ff083c13527fa6b05bd0eb6a6056a3305170058be32cb10", "changed typed allocation flag forwarding"
    assert typed_alloc[4:12] == bytes.fromhex("89 f2 48 8b 07 83 e2 07"), "changed typed allocation low flag preservation"
    heap_alloc = kernel_read(0xffffff8000369360, 0x370)
    assert hashlib.sha256(heap_alloc).hexdigest() == "fd65e2351139e6231be769f673b75b18b9cfd41a275750b43ddc5973955b4e97", "changed heap zone/large allocation forwarding"
    assert hashlib.sha256(typed_new).hexdigest() == "938c1b57ae18044138a66393284b04e4bd1c2249a9bd58200cd28b55d65a929e", "changed Boot typed object allocation policy"
    assert typed_new[0x20:0x25] == bytes.fromhex("ba 04 10 04 00"), "changed sized allocation zero flags"
    assert typed_new[0x2e:0x33] == bytes.fromhex("be 04 00 00 00"), "changed typed allocation zero flags"
    for name, length, digest in (
            (b"__ZNK8OSObject7releaseEv", 0x10, "da552c7fa83867904273c879beed50027c1c0325336862a6723a77e1d0b18e79"),
            (b"__ZNK8OSObject13taggedReleaseEPKv", 0x20, "82115e870c334d8be1cf952d56bb15fd54341a30cf51dc9a5103d132c34c99e1"),
            (b"__ZNK8OSObject13taggedReleaseEPKvi", 0xa0, "8e9a7872a69643787cfda92299764daac3e4427aaa60905a92968d226775b5f3"),
            (b"__ZNK8OSObject6retainEv", 0x10, "2be2f61d85bc0c7c51b0311f2501c1cfbe395d0681c6dc3939845f0b1f202649"),
            (b"__ZNK8OSObject14getRetainCountEv", 0x10, "d6251a4bc32d01d7a1fb85ec1b3e34cc197988eb3d24141855bd27976d51cc1a"),
            (b"__ZNK8OSObject12taggedRetainEPKv", 0x70, "df3131032cc056d05c27ffc14ef8f87960a1b087906110491d9b835a503e9b7c"),
            (b"__ZN8OSObjectnwEm", 0x30, "ede18fc0e04e045546d27a1994374beec6c7e3849165336ef54e69b03ae6c8fc"),
            (b"__ZN8OSObjectC2EPK11OSMetaClass", 0x20, "3bff5fda79db9a259910ce7c7bd2abc320e4f802db12952733c6cd5172127e8c"),
            (b"__ZNK11OSMetaClass19instanceConstructedEv", 0x30, "7da7d572ec33568cb147606982d6f62b83c2cdc233e7a1cfe4315de6703918c7"),
            (b"__ZN8OSObjectD2Ev", 0x10, "f87c6d05828374250e441fa1f70966f19d575146c67e3cff53439197774cf85b"),
            (b"__ZNK11OSMetaClass18instanceDestructedEv", 0x90, "0d67e188d3f4ceada292a22f4fe66ae4a8396f2497702f5466da893dd7c804a0"),
            (b"_IOSimpleLockFree", 0x50, "7924e21ddc26f4a79618ebad26be0c6bd8867a2aac4a0d07d447fe2df35c62c7"),
            (b"_lck_spin_free", 0x50, "a861440db5510d1c95c91eb5d9712428843a199e1e66042642b4fa07ffbf6a66"),
            (b"_lck_spin_destroy", 0x30, "9d1007a28ffb8ffddfbb2c3027d2cd860400926b65dd5f4a62907f412d220168"),
            (b"__ZN8OSObject4initEv", 0x10, "3d87417deea9934d463ef6b95f57292d339948cf6376364374173546137dff8c")):
        address = symbols[name][0]
        assert min(v for v in kernel_defined_addresses if v > address) - address == length, "changed lock/object method boundary"
        assert hashlib.sha256(kernel_read(address, length)).hexdigest() == digest, "changed reviewed null-lock/base-init semantics"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV8OSObject"][0] + 16 + 0x88, 8))[0] == symbols[b"__ZN8OSObject4initEv"][0], "changed OSObject base init virtual"
    print("PASS Boot KC spin-lock free requires nonnull lock; base OSObject init returns true")
    object_new = symbols[b"__ZN8OSObjectnwEm"][0]
    allocation = symbols[b"_kalloc_ext"][0]
    call = object_new + 0x1a
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == allocation, "changed OSObject new allocator edge"
    # Named-symbol span includes a separate unnamed large-allocation helper;
    # pin the reviewed region, not a claim that it is one complete function.
    assert hashlib.sha256(kernel_read(allocation, 0x370)).hexdigest() == "fd65e2351139e6231be769f673b75b18b9cfd41a275750b43ddc5973955b4e97", "changed reviewed allocation root/helper region"
    assert kernel_read(allocation + 0x123, 5) == bytes.fromhex("48 85 c0 74 49"), "changed small-allocation null-result branch"
    assert kernel_read(allocation + 0x171, 2) == bytes.fromhex("31 c0"), "changed allocator null-return register"
    print("PASS Boot KC OSObject allocator edge/region and conditional null propagation (zone policy pending)")
    for table, slot, method in (
            (b"__ZTV12IODMACommand", 0x118, b"__ZN12IODMACommand12cloneCommandEPv"),
            (b"__ZTV12IODMACommand", 0x90, b"__ZN12IODMACommand4freeEv"),
            (b"__ZTV12IODMACommand", 0x178, b"__ZN12IODMACommand14initWithRefConEPv"),
            (b"__ZTV12IODMACommand", 0x180, b"__ZN12IODMACommand21initWithSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapperS2_"),
            (b"__ZTV12IODMACommand", 0x128, b"__ZN12IODMACommand19setMemoryDescriptorEPK18IOMemoryDescriptorb"),
            (b"__ZTV12IODMACommand", 0x130, b"__ZN12IODMACommand21clearMemoryDescriptorEb"),
            (b"__ZTV12IODMACommand", 0x140, b"__ZN12IODMACommand7prepareEyybb"),
            (b"__ZTV12IODMACommand", 0x148, b"__ZN12IODMACommand8completeEbb"),
            (b"__ZTV25IOGeneralMemoryDescriptor", 0x1f8, b"__ZN25IOGeneralMemoryDescriptor8completeEj")):
        assert struct.unpack("<Q", kernel_read(symbols[table][0] + 16 + slot, 8))[0] == symbols[method][0], "changed base DMA/descriptor virtual identity"
    descriptor_operation = symbols[b"__ZNK25IOGeneralMemoryDescriptor19dmaCommandOperationEjPvj"][0]
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV25IOGeneralMemoryDescriptor"][0] + 16 + 0x130, 8))[0] == descriptor_operation, "changed general descriptor DMA operation virtual"
    assert hashlib.sha256(kernel_read(descriptor_operation, 0x8a0)).hexdigest() == "b86c29832730901e99dfd43d6c422cc2b6b77586b988d8e40a5d812410d9163e", "changed general descriptor DMA operation body/table"
    dma_operation_table = descriptor_operation + 0x888
    for index, offset in enumerate((0x21a, 0x35, 0x155, 0x181, 0xe9, 0x1c7)):
        assert dma_operation_table + struct.unpack("<i", kernel_read(dma_operation_table + index * 4, 4))[0] == descriptor_operation + offset, "changed DMA operation category jump table"
    assert kernel_read(descriptor_operation + 0x165, 6) == bytes.fromhex("66 f0 0f c1 47 34"), "changed descriptor active-DMA atomic increment"
    assert kernel_read(descriptor_operation + 0x215, 5) == bytes.fromhex("66 f0 ff 4f 34"), "changed descriptor active-DMA atomic decrement"
    for method, length, digest in (
            (b"__ZN12IODMACommand12cloneCommandEPv", 0xe0, "3dfcbe6d051b154d2826655cfafcf186237776218302cf500ba0d766bb750374"),
            (b"__ZN12IODMACommand14initWithRefConEPv", 0x50, "9bf2ee9c07c3677712965fc2168ed0fea43d9887de6461bf80a504c5fa9fb1c4"),
            (b"__ZN12IODMACommand4freeEv", 0xd0, "9d28f8d4336106353ff68e94b7636473253843289ace886c4f9ef43925422b67"),
            (b"__ZN12IODMACommand7prepareEyybb", 0x650, "233852a2a7af0fbf5b3107c282a6837bd3e74a20611022b47359585a0134b475"),
            (b"_kalloc_type_impl", 0x90, "5f34a636c2f1fbc91ff083c13527fa6b05bd0eb6a6056a3305170058be32cb10"),
            (b"__ZN12IODMACommand21initWithSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapperS2_", 0x60, "36cc23802bb6931657e908abf07c70352f24df054e3a7f432977c2dbe3a01e06"),
            (b"__ZN12IODMACommand16setSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapper", 0x290, "f61949ac55f2086536faaffc191f98a9535b29e01926a875e7741a6b27aca1ab"),
            (b"__ZN12IODMACommand17withSpecificationEPFbPS_NS_9Segment64EPvjEhyNS_14MappingOptionsEyjP8IOMapperS2_", 0x90, "d8d6abfba6270076a9f8e81f874592f600c9c0d75862ef3abb1cd55475de2453"),
            (b"__ZN12IODMACommand19setMemoryDescriptorEPK18IOMemoryDescriptorb", 0x1d0, "c3f7554a7c9a6dbb4357bccb00a3de4d2fa3d640fdfd141e5e92dace59cabee6"),
            (b"__ZN25IOGeneralMemoryDescriptor8completeEj", 0x3a0, "05696feca129a66a231bfdffc6173151ae05db56d52377b7b551f452c1bc06f1"),
            (b"__ZN12IODMACommand7walkAllEj", 0x380, "21f231d75f108aab5a00af400fa56e8dc64a47ba29119f6f1fdd37629537adda"),
            (b"__ZN12IODMACommand21clearMemoryDescriptorEb", 0x90, "b2e56b7f2a5154c2fc39d156c41faaf86ab8b54bb20a9d1b0566854515495d6b"),
            (b"__ZN12IODMACommand8completeEbb", 0x230, "7862d56c7f676b693648cda13d9973549ed700b71e093af739244d0dbae6edca")):
        assert hashlib.sha256(kernel_read(symbols[method][0], length)).hexdigest() == digest, "changed base DMA-command cleanup body"
    dma_complete = symbols[b"__ZN12IODMACommand8completeEbb"][0]
    dma_prepare = symbols[b"__ZN12IODMACommand7prepareEyybb"][0]
    assert kernel_read(dma_prepare + 0x52, 11) == bytes.fromhex("8b 47 68 44 8d 48 01 44 89 4f 68"), "changed DMA prepare reference increment before fallible work"
    allocator = symbols[b"_kalloc_type_impl"][0]
    typed_allocator = symbols[b"_IOMallocTypeImpl"][0]
    assert hashlib.sha256(kernel_read(typed_allocator, 0x20)).hexdigest() == "a4caf37ae7b57d98f187be6fd68606b525bfe6cb3282abc4d0c4464042b4e623", "changed reviewed typed IOMalloc wrapper"
    edge = kernel_read(typed_allocator + 0x15, 5)
    assert edge[0] == 0xe9 and typed_allocator + 0x1a + struct.unpack_from("<i", edge, 1)[0] == allocator, "changed typed IOMalloc external allocation edge"
    stub = system[0x1020a:0x10210]
    assert stub == bytes.fromhex("ff 25 a8 40 01 00"), "changed resource typed allocation import stub"
    encoded = struct.unpack_from("<Q", system, 0x242b8)[0]
    assert (encoded >> 30) & 3 == 0 and bases[0] + (encoded & 0x3fffffff) == typed_allocator, "changed resource typed allocation import identity"
    free_stub = system[0x101f2:0x101f8]
    assert free_stub[0:2] == bytes.fromhex("ff 25") and 0x101f8 + struct.unpack_from("<i", free_stub, 2)[0] == 0x24298, "changed typed event-pair free stub"
    encoded_free = struct.unpack_from("<Q", system, 0x24298)[0]
    assert (encoded_free >> 30) & 3 == 0 and bases[0] + (encoded_free & 0x3fffffff) == symbols[b"_IOFreeTypeImpl"][0], "changed typed event-pair free import identity"
    assert system[0x14b86753:0x14b86776] == bytes.fromhex("488bb3900000004885f6741748c7839000000000000000488d3d8f120600e87c9a48eb"), "changed resource event-pair clear-before-typed-free order"
    # This fixes allocator identity, not downstream zone no-failure policy.
    assert kernel_read(allocator + 9, 3) == bytes.fromhex("83 e2 07"), "changed external typed-allocation KPI flag mask"
    dma_free = symbols[b"__ZN12IODMACommand4freeEv"][0]
    assert kernel_read(dma_free + 0x9a, 8) == bytes.fromhex("48 c7 43 48 00 00 00 00"), "changed DMA free descriptor detach (not clearMemoryDescriptor)"
    dma_clone = symbols[b"__ZN12IODMACommand12cloneCommandEPv"][0]
    assert kernel_read(dma_clone + 0xad, 7) == bytes.fromhex("41 ff 92 80 01 00 00"), "changed clone SegmentOptions initializer dispatch"
    assert kernel_read(dma_clone + 0xb6, 7) == bytes.fromhex("74 05 4c 89 e8 eb 0c"), "changed clone initialization failure branch"
    assert kernel_read(dma_clone + 0xc4, 3) == bytes.fromhex("ff 50 28"), "changed failed clone release dispatch"
    dma_factory_name = b"__ZN12IODMACommand17withSpecificationEPFbPS_NS_9Segment64EPvjEhyNS_14MappingOptionsEyjP8IOMapperS2_"
    assert system[0x10072:0x10078] == bytes.fromhex("ff 25 20 40 01 00"), "changed accelerator DMA factory import stub"
    raw_dma_factory = struct.unpack_from("<Q", system, 0x24098)[0]
    assert raw_dma_factory >> 63 == 0 and (raw_dma_factory >> 30) & 3 == 0, "changed DMA factory cache level/auth"
    assert bases[0] + (raw_dma_factory & 0x3fffffff) == symbols[dma_factory_name][0], "changed accelerator DMA factory imported identity"
    dma_set = symbols[b"__ZN12IODMACommand19setMemoryDescriptorEPK18IOMemoryDescriptorb"][0]
    assert kernel_read(dma_set + 0x1a0, 5) == bytes.fromhex("be 01 00 00 03"), "changed DMA descriptor registration operation"
    assert kernel_read(dma_set + 0x1b0, 5) == bytes.fromhex("45 84 f6 75 aa"), "changed DMA registration ignored-result/autoprepare edge"
    assert kernel_read(dma_set + 0x18a, 5) == bytes.fromhex("be 01 00 00 00"), "changed DMA prepare-failure forced-clear argument"
    for call, method in ((0xffffff8000ad2f98, b"__ZN12IODMACommand7walkAllEj"),
                         (0xffffff8000ad338a, b"_kalloc_type_impl"),
                         (0xffffff8000ad33dc, b"__ZN12IODMACommand16setSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapper"),
                         (0xffffff8000ad363d, b"__ZN8IOMapper19waitForSystemMapperEv"),
                         (0xffffff8000ad3747, b"_lck_mtx_alloc_init"),
                         (0xffffff8000add8a5, b"_upl_commit_range"),
                         (0xffffff8000add8d9, b"_upl_abort_range"),
                         (0xffffff8000add8af, b"_upl_deallocate"),
                         (0xffffff8000ad42dd, b"_vm_page_free_list")):
        encoded = kernel_read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == symbols[method][0], "changed DMA/descriptor backing release edge"
    assert kernel_read(dma_complete, 7) == bytes.fromhex("8b 4f 68 85 c9 74 6e"), "changed DMA complete zero-count check"
    dma_clear = symbols[b"__ZN12IODMACommand21clearMemoryDescriptorEb"][0]
    assert kernel_read(dma_clear + 0x7c, 8) == bytes.fromhex("48 c7 43 48 00 00 00 00"), "changed DMA descriptor pointer clear"

    # This imported pointer addresses the vtable HEADER. The explicit base
    # call uses +0x860, not the object-vptr convention of header+16+slot.
    imported = struct.unpack_from("<Q", system, 0x14bcf088)[0]
    assert imported >> 63 == 0 and (imported >> 30) & 3 == 0, "changed user-client import cache level/auth"
    client_table = symbols[b"__ZTV12IOUserClient"][0]
    assert bases[0] + (imported & 0x3fffffff) == client_table, "changed inherited user-client table import"
    dispatch = symbols[b"__ZN12IOUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv"][0]
    assert struct.unpack("<Q", kernel_read(client_table + 0x860, 8))[0] == dispatch, "changed inherited external dispatch slot"
    assert hashlib.sha256(kernel_read(dispatch, 0x3be)).hexdigest() == "5a00aca7334586415dc7bcd96be5fbae8243436eaffb9f57f7ffde2f9d017b7d", "changed inherited external dispatch instructions"
    assert kernel_read(dispatch + 0x1e0, 6) == bytes.fromhex("48 8b 01 48 85 c0"), "changed descriptor action load/test"
    assert kernel_read(dispatch + 0x1fc, 2) == bytes.fromhex("ff e0"), "changed descriptor action tail dispatch"

    # These Boot KC vtable entries are canonical pointers, NOT System KC
    # chained cache-level targets. Do not silently apply the latter decoder.
    for table, slot, method, length, digest in (
            (b"__ZTV18IOTimerEventSource", 0x128,
             b"__ZN18IOTimerEventSource11setWorkLoopEP10IOWorkLoop", 0x60,
             "c65344022bf8d9bc0d0b8e7531c46ebb93e5f1330b3c1b230cc950fd0eb76ff8"),
            (b"__ZTV18IOTimerEventSource", 0x158,
             b"__ZN18IOTimerEventSource7disableEv", 0x60,
             "dfdaf0a822e0cead05bf9fd7676a3aaa0a78cff67f3cd9c270be9ef5c006e573"),
            (b"__ZTV13IOCommandGate", 0x140,
             b"__ZN13IOEventSource9setActionEPFvP8OSObjectzE", 0x50,
             "8cdc3bc5b1db948a9d976aa06d9bcb318519e859a4fd655f687ce8870b7d04d6"),
            (b"__ZTV13IOCommandGate", 0x1b8,
             b"__ZN13IOCommandGate4initEP8OSObjectPFiS1_PvS2_S2_S2_E", 0x30,
             "168ec388c4b98c395fa7cb00389015d571f9153b32d6a717f8f41b9955c125b3"),
            (b"__ZTV10IOWorkLoop", 0x88, b"__ZN10IOWorkLoop4initEv", 0x1a0,
             "3f0dee6fd2d4c2cbf11c7fcd7b0788c9bf2abb3c08526ae060fe159dfb7367f7"),
            (b"__ZTV13IOCommandGate", 0x1c8,
             b"__ZN13IOCommandGate9runActionEPFiP8OSObjectPvS2_S2_S2_ES2_S2_S2_S2_", 0x280,
             "b99aaabcc82bfb3f8116857b0ceed68604d84a049fc2068cd36335bd3d8ace12"),
            (b"__ZTV13IOCommandGate", 0x1c0,
             b"__ZN13IOCommandGate10runCommandEPvS0_S0_S0_", 0x30,
             "e3e3a145d770ee11a0fe2424d9adda5df15c0be7c5fa458c3278a44d5ee7dd41"),
            (b"__ZTV10IOWorkLoop", 0x178, b"__ZN10IOWorkLoop8openGateEv", 0x70,
             "4474d3fed4f663608045d23044b0ab55df1259532e987df6ae9ee3c9c3ad9e8a"),
            (b"__ZTV10IOWorkLoop", 0x180, b"__ZN10IOWorkLoop9closeGateEv", 0x90,
             "423e960ba7f1dcd3e429a66e4fe1e83046e4eb9ec13d301178fe085736ff5d89"),
            (b"__ZTV10IOWorkLoop", 0x148, b"__ZN10IOWorkLoop17removeEventSourceEP13IOEventSource", 0x30,
             "d055def6065f00813231758928fc7c6a20c8383b7f8ff890f7353517cde22747"),
            (b"__ZTV18IOTimerEventSource", 0x1c0,
             b"__ZN18IOTimerEventSource4initEP8OSObjectPFvS1_PS_E", 0x50,
             "a7e670f9a67cfecffd99d5280e9841fa418f6f9281d45edef55f3a2598ce63bb"),
            (b"__ZTV18IOTimerEventSource", 0x218,
             b"__ZN18IOTimerEventSource13cancelTimeoutEv", 0x70,
             "ca6a6e9adbde9fcd3242f53c7c407c5dbc8b737620144c1ffc34f1aae08cb4ac"),
            (b"__ZTV22IOInterruptEventSource", 0x1e0,
             b"__ZN22IOInterruptEventSource23normalInterruptOccurredEPvP9IOServicei", 0x140,
             "7a10ac711e79fee59301c61844df895055cc3e47d20e083526ac0b361640e9bb"),
            (b"__ZTV18IOTimerEventSource", 0x1d8,
             b"__ZN18IOTimerEventSource12setTimeoutUSEj", 0x20,
             "521f14403d11af5a02a549db936f42356c779fc6914a43036c671eb1cf8caeb5")):
        target = symbols[method][0]
        assert struct.unpack("<Q", kernel_read(symbols[table][0] + 16 + slot, 8))[0] == target, \
            "changed kernel event-source virtual target"
        assert hashlib.sha256(kernel_read(target, length)).hexdigest() == digest, \
            "changed reviewed kernel event-source body"
    normal_body = kernel_read(symbols[b"__ZN22IOInterruptEventSource23normalInterruptOccurredEPvP9IOServicei"][0], 0x140)
    assert normal_body.count(bytes.fromhex("ff 43 54")) == 1, "changed pending interrupt increment"
    assert normal_body.count(bytes.fromhex("48 8b 7b 30 48 8b 07 ff 90 70 01 00 00")) == 1, \
        "changed workloop notification virtual"
    print("PASS Boot KC interrupt pending notification and microsecond timer base virtuals")
    cancel = symbols[b"__ZN18IOTimerEventSource13cancelTimeoutEv"][0]
    for offset, method in ((0x1e, b"_thread_call_cancel"), (0x25, b"_thread_call_cancel_wait")):
        instruction = kernel_read(cancel + offset, 5)
        assert instruction[0] == 0xe8 and cancel + offset + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[method][0], \
            "changed timer cancel versus cancel-wait branch target"
    assert kernel_read(cancel + 0x14, 4) == bytes.fromhex("f6 43 2a 02"), \
        "changed timer active-mode wait selector"
    print("PASS Boot KC timer cancel virtual and conditional cancel-wait dispatch")
    # The setup symbol window includes a five-entry jump table after code.
    # Fix both the reviewed window and data targets, not a linear decoding of
    # that table as instructions. Init's further virtual delegation is pending.
    for method, length, digest in (
            (b"__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E", 0xc0,
             "79088c15da63def57438cdb0a553488e577861f0100f3a0c442a484585a51d5a"),
            (b"__ZN18IOTimerEventSource4initEjP8OSObjectPFvS1_PS_E", 0x20,
             "191a549b208c6e8133230403a4342f3f15b278a09ce9f57652b7e9a33f2dd046"),
            (b"__ZN18IOTimerEventSource14setTimeoutFuncEv", 0x120,
             "031283000cf6d490d994506e758769c7f9712765b731b2d9b03f93dc42dfb13c")):
        assert hashlib.sha256(kernel_read(symbols[method][0], length)).hexdigest() == digest, \
            "changed timer factory/options/setup window"
    setup = symbols[b"__ZN18IOTimerEventSource14setTimeoutFuncEv"][0]
    table = setup + 0x104
    for index, offset in enumerate((0x70, 0xb6, 0xa4, 0xad, 0x9b)):
        assert table + struct.unpack("<i", kernel_read(table + 4 * index, 4))[0] == setup + offset, \
            "changed timer priority jump-table target"
    factory = symbols[b"__ZN18IOTimerEventSource16timerEventSourceEP8OSObjectPFvS1_PS_E"][0]
    assert kernel_read(factory + 0x8d, 5) == bytes.fromhex("be 01 00 00 00"), "changed default timer options"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV18IOTimerEventSource"][0] + 16 + 0x220, 8))[0] == symbols[b"__ZN18IOTimerEventSource4initEjP8OSObjectPFvS1_PS_E"][0], \
        "changed timer options init virtual"
    print("PASS Boot KC default timer factory/options and setup jump table")
    timer_init = symbols[b"__ZN18IOTimerEventSource4initEP8OSObjectPFvS1_PS_E"][0]
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV18IOTimerEventSource"][0] + 16 + 0x1b8, 8))[0] == setup, \
        "changed timer init setup virtual"
    call = timer_init + 9
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE"][0], \
        "changed timer base event-source init edge"
    assert kernel_read(timer_init + 0x18, 6) == bytes.fromhex("ff 90 b8 01 00 00"), \
        "changed timer setup dispatch instruction"
    print("PASS Boot KC timer init delegation reaches pinned setup virtual")
    timeout = symbols[b"__ZN18IOTimerEventSource17timeoutAndReleaseEPvS0_"][0]
    body = kernel_read(timeout, 0x120)
    assert hashlib.sha256(body).hexdigest() == "aea398e58d775cac636c1c9275b989e05b4eb8322ccfeb71c8c0434afbc9e2dc", \
        "changed reviewed passive timer callback body"
    for offset, instruction in (
            (0x4c, "ff 90 80 01 00 00"),  # workloop gate entry
            (0x7f, "44 39 20"),           # generation comparison
            (0xf0, "ff 90 78 01 00 00"), # workloop gate exit
            (0x117, "ff 60 28")):        # source release tail
        assert kernel_read(timeout + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed passive timer generation/gate/release instruction"
    print("PASS Boot KC passive timer generation, workloop gate and release body")
    for method, offset, opcode, target in (
            (b"__ZN10IOWorkLoop9closeGateEv", 0x23, 0xe8, b"_IOLockLock"),
            (b"__ZN10IOWorkLoop8openGateEv", 0x5c, 0xe9, b"_IOLockUnlock")):
        call = symbols[method][0] + offset
        instruction = kernel_read(call, 5)
        assert instruction[0] == opcode and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[target][0], \
            "changed workloop recursive gate mutex edge"
    print("PASS Boot KC recursive workloop gate and removal delegation bodies")
    removal = symbols[b"__ZN10IOWorkLoop17removeEventSourceEP13IOEventSource"][0]
    for offset, encoded in ((0x4, "48 89 f2"),
                            (0x7, "48 8b 7f 20"),
                            (0xe, "48 8b 80 c0 01 00 00"),
                            (0x15, "be 01 00 00 00"),
                            (0x20, "ff e0")):
        expected = bytes.fromhex(encoded)
        assert kernel_read(removal + offset, len(expected)) == expected, \
            "changed synchronous remove delegation/operation selector"
    print("PASS Boot KC removal delegates operation 1 and source to control gate")
    command = symbols[b"__ZN13IOCommandGate10runCommandEPvS0_S0_S0_"][0]
    assert kernel_read(command + 0x13, 11) == bytes.fromhex("48 8b 77 20 48 8b 80 c8 01 00 00"), \
        "changed command stored-action/runAction virtual dispatch"
    print("PASS Boot KC command-gate removal wrapper preserves action delegation")
    action = symbols[b"__ZN13IOCommandGate9runActionEPFiP8OSObjectPvS2_S2_S2_ES2_S2_S2_S2_"][0]
    for offset, instruction in ((0x43, "ff 90 80 01 00 00"),
                                (0xa5, "41 ff d7"),
                                (0x100, "ff 90 78 01 00 00"),
                                (0x155, "ff 90 90 01 00 00")):
        assert kernel_read(action + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed command action gate/invocation/sleep instruction"
    print("PASS Boot KC command action gated invocation and disabled-gate sleep body")
    maintenance = symbols[b"__ZN10IOWorkLoop13_maintRequestEPvS0_S0_S0_"][0]
    assert hashlib.sha256(kernel_read(maintenance, 0x290)).hexdigest() == \
        "c442e1b551f044431ce91ab6d9a0288f45de5571d4557c3817130109f5ece1fb", \
        "changed workloop maintenance add/remove body"
    for offset, instruction in ((0x236, "ff 90 28 01 00 00"),
                                (0x244, "ff 90 30 01 00 00"),
                                (0x250, "ff 50 28")):
        assert kernel_read(maintenance + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed source detach/next-clear/release order"
    print("PASS Boot KC maintenance removal detach and reference-release body")
    workloop_table = symbols[b"__ZTV10IOWorkLoop"][0]
    assert struct.unpack("<Q", kernel_read(workloop_table + 16 + 0x118, 8))[0] == maintenance, \
        "changed base workloop maintenance action virtual"
    workloop_init = symbols[b"__ZN10IOWorkLoop4initEv"][0]
    for offset, instruction in ((0xe6, "4c 8b b8 18 01 00 00"),
                                (0x112, "4c 89 fa"),
                                (0x11f, "4c 89 73 20")):
        assert kernel_read(workloop_init + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed workloop control-gate maintenance binding instruction"
    print("PASS Boot KC base workloop initialization binds maintenance action")
    allocator = symbols[b"__ZNK13IOCommandGate9MetaClass5allocEv"][0]
    assert hashlib.sha256(kernel_read(allocator, 0x80)).hexdigest() == \
        "73955144ac7f8379c9117469d11186a3c0f61d1d121055713e6fcd754c738c41", \
        "changed command-gate allocator window"
    # Next symbol includes a separate unnamed initializer after this window.
    lea = allocator + 0x4b
    instruction = kernel_read(lea, 7)
    assert instruction[:3] == bytes.fromhex("48 8d 0d") and \
        lea + 7 + struct.unpack_from("<i", instruction, 3)[0] == symbols[b"__ZTV13IOCommandGate"][0] + 16, \
        "command-gate allocator no longer installs base vtable"
    init = symbols[b"__ZN13IOCommandGate4initEP8OSObjectPFiS1_PvS2_S2_S2_E"][0]
    call = init + 9
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE"][0], \
        "changed command-gate inherited init target"
    print("PASS Boot KC command-gate allocator vtable and inherited init edge")
    gate_factory = symbols[b"__ZN13IOCommandGate11commandGateEP8OSObjectPFiS1_PvS2_S2_S2_E"][0]
    assert hashlib.sha256(kernel_read(gate_factory, 0x60)).hexdigest() == \
        "7295677423d972b60335660a75fed45252f4540a645614dc97b2e5d49ab0c456", "changed command gate factory body"
    assert system[0x10468:0x1046e] == bytes.fromhex("ff 25 72 41 01 00"), "changed display gate factory import stub"
    factory_raw = struct.unpack_from("<Q", system, 0x245e0)[0]
    assert (factory_raw >> 30) & 3 == 0 and factory_raw >> 63 == 0, "unexpected gate factory cache level/auth"
    assert bases[0] + (factory_raw & 0x3fffffff) == gate_factory, "changed display gate factory imported identity"
    for offset, encoded in ((0x1e, "ff 90 88 00 00 00"),
                            (0x38, "ff 91 b8 01 00 00"),
                            (0x4d, "ff 50 28")):
        expected = bytes.fromhex(encoded)
        assert kernel_read(gate_factory + offset, len(expected)) == expected, "changed gate factory allocator/init/failure release edge"
    print("PASS paired KC display gate factory import and complete construction body")
    assert system[0x10138:0x1013e] == bytes.fromhex("ff 25 62 40 01 00"), "changed display workloop factory stub"
    display_workloop_raw = struct.unpack_from("<Q", system, 0x241a0)[0]
    assert (display_workloop_raw >> 30) & 3 == 0 and display_workloop_raw >> 63 == 0, "unexpected display workloop factory cache level/auth"
    assert bases[0] + (display_workloop_raw & 0x3fffffff) == symbols[b"__ZN10IOWorkLoop8workLoopEv"][0], "changed shared/private display workloop factory identity"
    base_init = symbols[b"__ZN13IOEventSource4initEP8OSObjectPFvS1_zE"][0]
    assert hashlib.sha256(kernel_read(base_init, 0x70)).hexdigest() == \
        "f975c99f5099be0529c344faf80ba56970164feafab786803119f95ba64e0441", \
        "changed inherited event-source owner/action init"
    assert kernel_read(base_init + 0x12, 4) == bytes.fromhex("48 89 5f 18"), "changed event-source owner store"
    assert kernel_read(base_init + 0x1c, 6) == bytes.fromhex("ff 90 40 01 00 00"), "changed event-source action setter virtual"
    print("PASS Boot KC inherited owner storage and effective command action setter")
    # gMetaClass is runtime-initialized zero storage in this file. Resolve the
    # initializer's symbolic reference, not a fictitious on-disk object vptr.
    load = workloop_init + 0xed
    instruction = kernel_read(load, 7)
    metaclass = symbols[b"__ZN13IOCommandGate10gMetaClassE"][0]
    assert instruction[:3] == bytes.fromhex("48 8b 05") and \
        load + 7 + struct.unpack_from("<i", instruction, 3)[0] == metaclass, \
        "changed workloop control-gate metaclass reference"
    assert kernel_read(metaclass, 8) == b"\0" * 8, "changed on-disk metaclass initialization state"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTVN13IOCommandGate9MetaClassE"][0] + 16 + 0x88, 8))[0] == allocator, \
        "changed command-gate metaclass allocator virtual"
    print("PASS Boot KC control-gate metaclass reference and allocator identity (runtime init pending)")
    # Unnamed metaclass initializer follows the allocator's separate window.
    initializer = allocator + 0x80
    assert hashlib.sha256(kernel_read(initializer, 0x70)).hexdigest() == \
        "2d431a9f495d3d9cd94bd4af3c6e6840f7cc0f6b45063ddda97fe1a057f64da2", \
        "changed command-gate metaclass initializer window"
    lea = initializer + 0x51
    instruction = kernel_read(lea, 7)
    assert instruction[:3] == bytes.fromhex("48 8d 05") and \
        lea + 7 + struct.unpack_from("<i", instruction, 3)[0] == symbols[b"__ZTVN13IOCommandGate9MetaClassE"][0] + 16, \
        "changed metaclass initializer vtable address"
    write = initializer + 0x58
    instruction = kernel_read(write, 7)
    assert instruction[:3] == bytes.fromhex("48 89 05") and \
        write + 7 + struct.unpack_from("<i", instruction, 3)[0] == metaclass, \
        "changed metaclass initializer vptr store"
    print("PASS Boot KC metaclass initializer writes the declared allocator vtable")
    stub = 0x10138
    assert system[stub:stub + 6] == bytes.fromhex("ff 25 62 40 01 00"), "changed accelerator workloop factory stub"
    pointer = stub + 6 + struct.unpack_from("<i", system, stub + 2)[0]
    raw = struct.unpack_from("<Q", system, pointer)[0]
    assert (raw >> 30) & 3 == 0 and raw >> 63 == 0, "unexpected workloop factory cache/auth"
    assert bases[0] + (raw & 0x3fffffff) == symbols[b"__ZN10IOWorkLoop8workLoopEv"][0], \
        "accelerator factory no longer resolves to IOWorkLoop::workLoop"
    call = 0x14ba02c7
    assert system[call] == 0xe8 and call + 5 + struct.unpack_from("<i", system, call + 1)[0] == stub, \
        "changed accelerator workloop construction call"
    assert system[call + 5:call + 12] == bytes.fromhex("49 89 86 f0 00 00 00"), "changed accelerator workloop store"
    print("PASS paired KC accelerator workloop factory import and field store (full lifecycle pending)")
    factory = symbols[b"__ZN10IOWorkLoop8workLoopEv"][0]
    assert hashlib.sha256(kernel_read(factory, 0xb0)).hexdigest() == \
        "f29215957d359f5d3c645b2752a35c06301834d22c2755190eb0e9e8f9709284", \
        "changed reviewed workloop factory body"
    lea = factory + 0x54
    instruction = kernel_read(lea, 7)
    assert instruction[:3] == bytes.fromhex("48 8d 05") and \
        lea + 7 + struct.unpack_from("<i", instruction, 3)[0] == workloop_table + 16, \
        "workloop factory no longer installs base vtable"
    assert kernel_read(factory + 0x8d, 6) == bytes.fromhex("ff 90 88 00 00 00"), "changed workloop init dispatch"
    assert kernel_read(factory + 0x9d, 5) == bytes.fromhex("ff 50 28 31 db"), "changed failed workloop init release/null result"
    print("PASS Boot KC concrete base workloop factory and failed-init cleanup")
    stub = 0x1051c
    assert system[stub:stub + 6] == bytes.fromhex("ff 25 ae 41 01 00"), "changed stop block import"
    pointer = stub + 6 + struct.unpack_from("<i", system, stub + 2)[0]
    raw = struct.unpack_from("<Q", system, pointer)[0]
    assert (raw >> 30) & 3 == 0 and raw >> 63 == 0, "unexpected stop block import cache/auth"
    block_api = symbols[b"__ZN10IOWorkLoop14runActionBlockEU13block_pointerFivE"][0]
    assert bases[0] + (raw & 0x3fffffff) == block_api, "changed stop block API identity"
    for start, length, digest in (
            (block_api, 0x40, "a38a67b32da3d99f873a5c26a175e11b20292dd190e22c189df7f5a815c15472"),
            (block_api + 0x40, 0x10, "353d84cf14acdfbc12e30defec09060ca37f6116ecc2aedb54919200c05b601b"),
            (0xffffff8000ac9be0, 0x60, "42ddad5ca2b1ca4851b501da78fc0a811a324aa38b798c6d591b2f08195b6100")):
        assert hashlib.sha256(kernel_read(start, length)).hexdigest() == digest, "changed gated block execution body"
    assert struct.unpack("<Q", kernel_read(workloop_table + 16 + 0x1a0, 8))[0] == 0xffffff8000ac9be0, \
        "changed base workloop synchronous action virtual"
    print("PASS paired KC stop block API and synchronous base workloop action body")
    detach = symbols[b"__ZN18IOTimerEventSource11setWorkLoopEP10IOWorkLoop"][0]
    assert kernel_read(detach + 0x15, 6) == bytes.fromhex("ff 90 58 01 00 00"), "changed timer detach disable dispatch"
    assert kernel_read(detach + 0x1e, 4) == bytes.fromhex("48 89 5f 30"), "changed timer workloop-pointer store"
    disable = symbols[b"__ZN18IOTimerEventSource7disableEv"][0]
    for offset, name in ((0x1e, b"_thread_call_cancel"), (0x25, b"_thread_call_cancel_wait")):
        call = disable + offset
        instruction = kernel_read(call, 5)
        assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == symbols[name][0], \
            "changed timer disable cancel branch"
    print("PASS Boot KC timer detach disables before clearing workloop")
    wake = symbols[b"__ZN18IOTimerEventSource10wakeAtTimeEjyy"][0]
    assert hashlib.sha256(kernel_read(wake, 0x130)).hexdigest() == \
        "28931d5cf72ac20f040ffb5fed17e87ba6c3eacc876e81c3d3ac05f18992ca2c", \
        "changed reviewed timer scheduling/retention body"
    for offset, instruction in ((0x6a, "ff 50 20"), (0x74, "ff 50 20"),
                                (0xbf, "ff 50 28"), (0xc9, "ff 50 28")):
        assert kernel_read(wake + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed passive timer schedule retain/release instruction"
    print("PASS Boot KC passive timer schedule-time reference pairing body")
    cancel = symbols[b"_thread_call_cancel"][0]
    # The next named symbol includes an unnamed locked helper: do not merge
    # the public wrapper and helper into one alleged function body.
    for start, length, digest in (
            (cancel, 0x120, "1744ae5843301d3f9f35d6e75d790c12866f84823dbceb19bd8b0bad8b215913"),
            (cancel + 0x120, 0x1f0, "04ebbd1202258d02da097a28b70152d3307dc5317d8d8214bc78bde5cfcf702d")):
        assert hashlib.sha256(kernel_read(start, length)).hexdigest() == digest, \
            "changed reviewed non-waiting thread-call cancellation window"
    call = cancel + 0x7b
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == cancel + 0x120, \
        "changed public cancellation to locked-helper edge"
    print("PASS Boot KC cancellation wrapper and separate locked-helper windows (not a drain proof)")
    object_free = symbols[b"__ZN8OSObject4freeEv"][0]
    assert hashlib.sha256(kernel_read(object_free, 0x30)).hexdigest() == \
        "6b5b497597ec0a7094328a70abfd1f3c877fe7f1e07f32a017bc104382ef37cd", \
        "changed reviewed OSObject free wrapper window"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV8OSObject"][0] + 0xa0, 8))[0] == object_free, \
        "changed OSObject base free dispatch slot"
    assert kernel_read(object_free + 0x28, 3) == bytes.fromhex("ff 60 08"), \
        "changed OSObject free deleting-destructor dispatch"
    print("PASS Boot KC OSObject base free dispatch and deleting-destructor edge (callee review pending)")
    object_delete = symbols[b"__ZN8OSObjectdlEPvm"][0]
    assert hashlib.sha256(kernel_read(object_delete, 0x40)).hexdigest() == \
        "04b6869b3dc3784361e720a8287053f5f525011e12bbf9cd0b9fd8bcf8a609ae", \
        "changed reviewed OSObject sized delete wrapper"
    print("PASS Boot KC sized object-delete wrapper (allocator callee pending)")
    source_workloop = symbols[b"__ZNK13IOEventSource11getWorkLoopEv"][0]
    assert kernel_read(source_workloop, 0x10) == bytes.fromhex("55 48 89 e5 48 8b 47 30 5d c3 66 0f 1f 44 00 00"), \
        "changed reviewed event-source workloop getter body"
    assert struct.unpack("<Q", kernel_read(symbols[b"__ZTV18IOTimerEventSource"][0] + 16 + 0x168, 8))[0] == source_workloop, \
        "changed effective timer attached-workloop getter slot"
    print("PASS Boot KC timer effective attached-workloop getter (not callback slot)")
    cancel_wait = symbols[b"_thread_call_cancel_wait"][0]
    assert hashlib.sha256(kernel_read(cancel_wait, 0x3d0)).hexdigest() == \
        "ab79f907dfbfeb04b2723874f2299984cdc722577b7c745328f5d916f5c84d48", \
        "changed reviewed cancellation-wait body"
    for offset, instruction in ((0xd6, "40 f6 c6 20"),
                                (0xdf, "84 c0"),
                                (0x173, "4c 8b 73 70"),
                                (0x177, "4c 39 73 78"),
                                (0x1a6, "80 4b 42 02")):
        assert kernel_read(cancel_wait + offset, len(bytes.fromhex(instruction))) == bytes.fromhex(instruction), \
            "changed cancellation-wait mode/result/snapshot/waiter branch"
    print("PASS Boot KC conditional cancellation-wait and fixed counter snapshot (resubmission not excluded)")
    invoke = 0xffffff8000ad01b0
    assert hashlib.sha256(kernel_read(invoke, 0x160)).hexdigest() == \
        "db682f5eff10d5a738d4dc74d880b75b3718960bd2716644dd9f7f0700ba11f9", \
        "changed reviewed timer action invocation helper window"
    call = timeout + 0xc1
    instruction = kernel_read(call, 5)
    assert instruction[0] == 0xe8 and call + 5 + struct.unpack_from("<i", instruction, 1)[0] == invoke, \
        "changed passive timer invocation helper edge"
    assert kernel_read(timeout + 0xb0, 4) == bytes.fromhex("48 8b 4b 18"), \
        "changed callback owner argument load"
    assert kernel_read(invoke + 0x4a, 9) == bytes.fromhex("48 89 df 48 89 d6 41 ff d4"), \
        "changed direct timer action owner/source invocation"
    print("PASS Boot KC passive timer owner forwarding and action helper (owner lifetime not established)")
    assert system[0x10132:0x10138] == bytes.fromhex("ff 25 60 40 01 00"), "changed atomic import stub"
    raw = struct.unpack_from("<Q", system, 0x24198)[0]
    assert (raw >> 30) & 3 == 0 and raw >> 63 == 0, "unexpected atomic import cache level/auth"
    address = bases[0] + (raw & 0x3fffffff)
    assert address == symbols[b"_OSIncrementAtomic"][0], "atomic import does not resolve to OSIncrementAtomic"
    assert system[0x1012c:0x10132] == bytes.fromhex("ff 25 5e 40 01 00"), "changed decrement import stub"
    decrement_raw = struct.unpack_from("<Q", system, 0x24190)[0]
    assert (decrement_raw >> 30) & 3 == 0 and decrement_raw >> 63 == 0, "unexpected decrement cache level/auth"
    decrement_address = bases[0] + (decrement_raw & 0x3fffffff)
    assert decrement_address == symbols[b"_OSDecrementAtomic"][0], "busy decrement import does not resolve to OSDecrementAtomic"
    decrement_locations = [f + decrement_address - v for v, f, size in segments
                           if v <= decrement_address and decrement_address + 15 <= v + size]
    assert len(decrement_locations) == 1, "unmapped decrement implementation"
    decrement_offset = decrement_locations[0]
    assert boot[decrement_offset:decrement_offset + 15] == bytes.fromhex(
        "55 48 89 e5 b8 ff ff ff ff f0 0f c1 07 5d c3"), "changed atomic decrement"
    for stub, name in ((0x101a4, b"__ZN15IORegistryEntry18getRegistryEntryIDEv"),
                       (0x101ce, b"_kernel_debug"), (0x10012, b"_IOLockLock"),
                       (0x10018, b"_IOLockUnlock"), (0x10c30, b"_assert_wait_deadline"),
                       (0x10366, b"_thread_block"), (0x100d8, b"_clock_interval_to_deadline")):
        assert system[stub:stub + 2] == b"\xff\x25", "changed lock notification import stub"
        pointer = stub + 6 + struct.unpack_from("<i", system, stub + 2)[0]
        raw_pointer = struct.unpack_from("<Q", system, pointer)[0]
        assert (raw_pointer >> 30) & 3 == 0 and raw_pointer >> 63 == 0, "unexpected lock notification cache level/auth"
        assert bases[0] + (raw_pointer & 0x3fffffff) == symbols[name][0], "changed lock notification import identity"
    wake_stub = 0x10cae
    assert system[wake_stub:wake_stub + 2] == b"\xff\x25", "changed wakeup import stub"
    wake_pointer = wake_stub + 6 + struct.unpack_from("<i", system, wake_stub + 2)[0]
    wake_raw = struct.unpack_from("<Q", system, wake_pointer)[0]
    assert (wake_raw >> 30) & 3 == 0 and wake_raw >> 63 == 0, "unexpected wakeup cache level/auth"
    assert bases[0] + (wake_raw & 0x3fffffff) == symbols[b"_thread_wakeup_prim"][0], \
        "signalStamp import does not resolve to thread_wakeup_prim"
    locations = [f + address - v for v, f, size in segments if v <= address and address + 15 <= v + size]
    assert len(locations) == 1, "unmapped atomic implementation"
    offset = locations[0]
    assert boot[offset:offset + 15] == bytes.fromhex(
        "55 48 89 e5 b8 01 00 00 00 f0 0f c1 07 5d c3"), "changed atomic increment"
    print("PASS busy/termination atomics, lock notifications and event wakeup imports across SystemKC/BootKC")


def check(path, boot_path=None):
    image = pathlib.Path(path).read_bytes()
    # Identity is checked before parsing this deliberately version-specific
    # fixture. An unknown KC must be reviewed, never silently accepted.
    assert hashlib.sha256(image).hexdigest() == KC_SHA256, "unreviewed KC identity"
    # Complete reviewed inherited stop and its captured-owner block. These
    # hashes do not certify called virtuals or runActionBlock synchronization.
    for start, length, digest in (
            (0x14ba1a7c, 0x43f, "5a5b95178a1b0bd70d3699730f0fcadc69f4449232a74a1e44231ea94172820b"),
            (0x14ba1ebb, 0x2d7, "bfcad84d6dc88c187f2478166bc752a66dbba41d2d4a123a4d8be8d8653232b1")):
        assert hashlib.sha256(image[start:start + length]).hexdigest() == digest, "changed accelerator stop/block body"
    stop_block = image[0x14ba1ebb:0x14ba2192]
    assert stop_block.count(bytes.fromhex("ff 90 48 01 00 00")) == 12, "changed stop-block removal inventory"
    assert image[0x14ba1d7d:0x14ba1d8b] == bytes.fromhex("ff 50 28 49 c7 86 f0 00 00 00 00 00 00 00"), \
        "changed stop workloop release/clear sequence"
    print("PASS complete inherited accelerator stop/block and twelve source removal calls")
    entries = []
    for command, offset in commands(image, 0):
        if command == 0x80000035:
            _, _, _, file_offset, name_offset, _ = struct.unpack_from("<IIQQII", image, offset)
            name_start = offset + name_offset
            if image[name_start:image.index(0, name_start)] == IDENTIFIER:
                entries.append(file_offset)
    assert len(entries) == 1, "missing/ambiguous IOAccel fileset"
    segments = []
    executable_segments = []
    symtab = None
    uuids = []
    for command, offset in commands(image, entries[0]):
        if command == 0x19:
            fields = struct.unpack_from("<II16sQQQQIIII", image, offset)
            segments.append((fields[3], fields[5], fields[6]))
            if fields[8] & 4:
                executable_segments.append((fields[3], fields[5], fields[6]))
        elif command == 2:
            assert symtab is None, "duplicate symbol table"
            symtab = struct.unpack_from("<6I", image, offset)[2:]
        elif command == 0x1b:
            assert struct.unpack_from("<I", image, offset + 4)[0] == 24, "malformed LC_UUID"
            uuids.append(image[offset + 8:offset + 24])
    assert uuids == [IOACCEL_UUID], "unreviewed IOAcceleratorFamily2 UUID"
    assert symtab is not None, "missing embedded symbol table"
    symbol_offset, count, string_offset, string_size = symtab
    base_client_symbols = {method for table in (
        SURFACE_METHODS, DEVICE_METHODS, SHARED_METHODS, GL_CONTEXT_METHODS,
        GL_DRAWABLE_METHODS, SURFACE_MTL_METHODS, MEMORY_INFO_METHODS,
        DISPLAY_PIPE_METHODS)
                           for method, _ in table if method is not None}
    wanted_symbols = {
        *CONTRACTS, *SCRUB_BODIES, *LOCK_COPIES, *EVENT_OWNER_BODIES,
        *BASE_CLIENT_BODIES, *RESOURCE_PAGING_BODIES,
        *ACCELERATOR_FINALIZE_BODIES, *base_client_symbols,
        SHARED_VTABLE, RESOURCE_VTABLE,
        "__ZTV18IOAccelDisplayPipe", "__ZTV24IOAccelLegacyDisplayPipe",
        "__ZTV19IOAccelCommandQueue", "__ZTV15IOAccelContext2",
        "__ZTV17IOAccel2DContext2", "__ZTV14IOAccelSurface",
        "__ZTV20IOAccelLegacySurface", "__ZTV14IOAccelDevice2",
        "__ZTV17IOAccelGLContext2", "__ZTV27IOAccelGLDrawableUserClient",
        "__ZTV17IOAccelSurfaceMTL",
        "__ZTV27IOAccelMemoryInfoUserClient",
        "__ZTV21IOAccelDisplayMachine", "__ZTV27IOAccelLegacyDisplayMachine",
        "__ZTV29IOAccelDisplayPipeUserClient2",
        "__ZN19IOAccelCommandQueue20sCommandQueueMethodsE",
        "__ZN15IOAccelContext215sContextMethodsE",
        "__ZN17IOAccel2DContext217s2DContextMethodsE",
        "__ZN14IOAccelSurface15sSurfaceMethodsE",
        "__ZN14IOAccelSurface20sSignalEventDispatchE",
        "__ZZN14IOAccelSurface14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE19newResourceDispatch",
        "__ZN14IOAccelDevice214sDeviceMethodsE",
        "__ZN24IOAccelSharedUserClient214sSharedMethodsE",
        "__ZZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE7methods",
        "__ZZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE15gather_dispatch",
        "__ZN29IOAccelDisplayPipeUserClient219sDisplayMethodDescsE",
        "__ZZN17IOAccelGLContext212contextStartEvE19methodDispatchDescs",
        "__ZL25sGLContextMethodsDispatch",
        "__ZN27IOAccelGLDrawableUserClient15sMethodDispatchE",
        "__ZN17IOAccelSurfaceMTL15sSurfaceMethodsE",
        "__ZN17IOAccelSurfaceMTL20sSignalEventDispatchE",
        "__ZZN17IOAccelSurfaceMTL14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE19newResourceDispatch",
        "__ZN17IOAccelGLContext211contextStopEv",
        "__ZN17IOAccelGLContext226sleepForSwapCompleteNoLockEj",
        "__ZN17IOAccelGLContext228addVendorSurfaceRequiredBitsEy",
        "__ZN14IOAccelSurface21copyFromBufferWithCPUEiiiijjP16IOAccelResource2P16IOAccelSysMemoryjyj",
        "__ZN20IOAccelLegacySurface25present_surface_with_flipEj",
        "__ZN27IOAccelLegacyDisplayMachine16setup_fullscreenEP20IOAccelLegacySurface",
        "__ZN17IOAccelSurfaceMTL22isSurfaceSizeSupportedEss",
        "__ZN28IOSurfaceMTLSharedEventFence15withSharedEventEP20IOSurfaceSharedEventy",
        "__ZN23IOAccelSharedEventFence15withSharedEventEP20IOSurfaceSharedEventyP22IOGraphicsAccelerator2P17IOAccelSubmitter2",
        "__ZN19IOAccelFenceMachine13addEventFenceEP17IOAccelSubmitter2P17IOAccelEventFence",
        "__ZN22IOGraphicsAccelerator218unwireAllVidMemoryEv",
        "__ZN22IOGraphicsAccelerator218unwireAllSysMemoryEv",
        "__ZN19IOAccelCommandQueue16commandQueueStopEv",
        "__ZN19IOAccelCommandQueue11setPriorityE28eIOAccelCommandQueuePriority",
        "__ZTV16IOAccelMemoryMap", "__ZTV16IOAccelSysMemory",
        "__ZTV11IOAccelTask", "__ZTV24IOAccelSharedUserClient2",
        "__ZTV13IOAccelMemory", "__ZTV22IOGraphicsAccelerator2",
        "__ZN22IOGraphicsAccelerator223freeWaitToPrepareVidMapEP16IOAccelMemoryMapbb",
        "__ZNK16IOAccelMemoryMap9getLengthEv",
        EVENT_VTABLE, EVENT_FINISH, EVENT_WAIT, EVENT_CLEAN, EVENT_SIGNAL,
        EVENT_RESTART, EVENT_MERGE_EXCLUDING, EVENT_SET_STAMP, GET_DATA_BUFFER,
        EVENT_INIT, EVENT_COPY, EVENT_FINISH_UNLOCKED, EVENT_HARDWARE_ERROR,
        EVENT_DISABLE_STAMP_LOCKED, EVENT_ENABLE_STAMP, EVENT_DISABLE_STAMP,
    }
    matches = {name: [] for name in wanted_symbols}
    for index in range(count):
        name_offset, _, _, _, address = struct.unpack_from("<IBBHQ", image, symbol_offset + index * 16)
        assert name_offset < string_size, "invalid symbol string"
        start = string_offset + name_offset
        name = image[start:image.index(0, start, string_offset + string_size)].decode()
        if name in matches:
            matches[name].append(address)
    def address_of(name):
        assert len(matches[name]) == 1, f"missing/ambiguous {name}"
        return matches[name][0]

    def read(address, length):
        locations = [file_offset + address - virtual for virtual, file_offset, size in segments
                     if virtual <= address and address + length <= virtual + size]
        assert len(locations) == 1, f"unmapped/ambiguous address {address:#x}"
        offset = locations[0]
        return image[offset:offset + length]

    def direct_branch_offsets(start, length, target):
        """Pinned symbol-window E8/E9 locator, not an indirect-call proof."""
        body = read(start, length)
        found = []
        for offset in range(max(0, length - 4)):
            if body[offset] not in (0xe8, 0xe9):
                continue
            displacement = struct.unpack_from("<i", body, offset + 1)[0]
            if start + offset + 5 + displacement == target:
                found.append(offset)
        return found

    for name, expected in CONTRACTS.items():
        address = address_of(name)
        assert read(address, len(expected)) == expected, f"changed {name}"
        print(f"PASS {name} at {address:#x}")
    for name, (length, digest) in SCRUB_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, f"changed {name}"
    for name, (length, digest) in EVENT_OWNER_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, f"changed event owner lifecycle: {name}"
    for name, (length, digest) in BASE_CLIENT_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, \
            f"changed base-client/surface body: {name}"
    for name, (length, digest) in RESOURCE_PAGING_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, \
            f"changed inherited resource-paging/control body: {name}"
    for name, (length, digest) in ACCELERATOR_FINALIZE_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, \
            f"changed accelerator finalize/device-cache body: {name}"

    def indirect_vtable_call_sites(slot):
        found = set()
        for virtual, file_offset, size in executable_segments:
            body = image[file_offset:file_offset + size]
            for relative in range(max(0, size - 5)):
                # FF /2 with mod=10 and r/m selecting any GPR, followed by a
                # little-endian disp32.  An optional REX byte precedes FF and
                # therefore does not change this instruction-local match.
                if body[relative] == 0xff and 0x90 <= body[relative + 1] <= 0x97 and \
                        struct.unpack_from("<I", body, relative + 2)[0] == slot:
                    found.add(virtual + relative)
        return found

    for slot, expected in RESOURCE_SLOT_CALL_SITES.items():
        actual = indirect_vtable_call_sites(slot)
        assert actual == expected, \
            f"changed complete executable vtable-call inventory for slot {slot:#x}: {sorted(actual)}"
    print("PASS complete inherited resource paging-slot call-site inventory")

    all_resource_slot_sites = set().union(*RESOURCE_SLOT_CALL_SITES.values())
    classified_resource_slot_sites = set()
    for classification, sites in RESOURCE_SLOT_CALL_SITE_CLASSES.items():
        assert classified_resource_slot_sites.isdisjoint(sites), \
            f"duplicate resource-slot classification: {classification}"
        classified_resource_slot_sites.update(sites)
    assert classified_resource_slot_sites == all_resource_slot_sites, \
        "resource-slot classification is not an exact complete partition"
    assert {name: len(sites) for name, sites in RESOURCE_SLOT_CALL_SITE_CLASSES.items()} == {
        "admitted_or_control_descendant": 57,
        "retirement_or_teardown": 5,
        "shared_low_level_bridge": 11,
        "unrelated_receiver": 31,
    }, "changed resource-slot classification cardinality"
    print("PASS complete resource paging-slot admission/retirement/bridge partition")

    def check_dispatch_table(table_name, expected, digest):
        table = address_of(table_name)
        assert hashlib.sha256(read(table, len(expected) * 48)).hexdigest() == digest, \
            f"changed complete external dispatch table: {table_name}"
        for selector, (method, arguments) in enumerate(expected):
            entry = struct.unpack("<6Q", read(table + selector * 48, 48))
            assert entry[0] == 0 and entry[2:] == arguments, \
                f"changed selector {selector} arguments: {table_name}"
            if method is None:
                assert entry[1] == 0, f"changed special selector {selector} target: {table_name}"
            else:
                assert entry[1] >> 63 == 0 and (entry[1] >> 30) & 3 == 1 and \
                    entry[1] & 0x3fffffff == address_of(method), \
                    f"changed selector {selector} target: {table_name}"

    check_dispatch_table(
        "__ZN14IOAccelSurface15sSurfaceMethodsE", SURFACE_METHODS,
        "1cba789edda2960c27c1da7c89fbba97b21b70b4371c6a2df213f1655be6246a")
    check_dispatch_table(
        "__ZN14IOAccelDevice214sDeviceMethodsE", DEVICE_METHODS,
        "c5bf8567fbf60dbae2cf77e57d2514a56d5aace0ae5d2ab923808f69aaaff207")
    check_dispatch_table(
        "__ZN24IOAccelSharedUserClient214sSharedMethodsE", SHARED_METHODS,
        "ba76d2c8dca49a0f9e44703c005f92a19695a17dbc95f60bbb507d0d2ee147ea")

    def check_legacy_external_dispatch_table(table_name, expected, digest):
        table = address_of(table_name)
        assert hashlib.sha256(read(table, len(expected) * 24)).hexdigest() == digest, \
            f"changed complete legacy external dispatch table: {table_name}"
        for selector, (method, arguments) in enumerate(expected):
            entry = struct.unpack("<Q4I", read(table + selector * 24, 24))
            assert entry[1:] == arguments, \
                f"changed legacy selector {selector} arguments: {table_name}"
            assert entry[0] >> 63 == 0 and (entry[0] >> 30) & 3 == 1 and \
                entry[0] & 0x3fffffff == address_of(method), \
                f"changed legacy selector {selector} target: {table_name}"

    check_legacy_external_dispatch_table(
        "__ZZN17IOAccelGLContext212contextStartEvE19methodDispatchDescs",
        GL_CONTEXT_METHODS,
        "b0e84a5a26ba67226fa5514293cc60af71ce6daf85788eb98e9b57b12ca8c259")
    check_legacy_external_dispatch_table(
        "__ZN27IOAccelGLDrawableUserClient15sMethodDispatchE",
        GL_DRAWABLE_METHODS,
        "608a8a962b027791704ca9eac5acd9c30ea9e1eb3f9cd3581cf29f6f20977d89")
    check_legacy_external_dispatch_table(
        "__ZZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE15gather_dispatch",
        MEMORY_INFO_METHODS,
        "9a7180e937fe917fec5ca4273f58b79ff4ef9bdae8f9c350c79fdc7463bd0a3b")
    check_legacy_external_dispatch_table(
        "__ZN29IOAccelDisplayPipeUserClient219sDisplayMethodDescsE",
        DISPLAY_PIPE_METHODS,
        "fa48e879df1eeaaaab8a89f01cff91038e7b88414e9372fca451396187c9007a")
    check_dispatch_table(
        "__ZN17IOAccelSurfaceMTL15sSurfaceMethodsE", SURFACE_MTL_METHODS,
        "acbedfcc5220cc7de2fb393c5757210b78ec40e36545e2ee36a712859f29515f")

    gl_start = address_of("__ZN17IOAccelGLContext212contextStartEv")
    for offset, prefix, target in (
            (0x106, bytes.fromhex("48 8d 05"),
             address_of("__ZZN17IOAccelGLContext212contextStartEvE19methodDispatchDescs")),
            (0x10d, bytes.fromhex("48 89 05"), address_of("__ZL25sGLContextMethodsDispatch"))):
        instruction = read(gl_start + offset, 7)
        assert instruction[:3] == prefix and \
            gl_start + offset + 7 + struct.unpack_from("<i", instruction, 3)[0] == target, \
            "changed GL-context dispatch publication"
    gl_external = address_of(
        "__ZN17IOAccelGLContext214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv")
    assert read(gl_external + 4, 11) == bytes.fromhex(
        "89 f0 8d b0 00 ff ff ff 83 fe 05"), \
        "changed GL-context 0x100 selector normalization/bounds"
    assert read(gl_external + 0x11, 7) == bytes.fromhex("f6 87 98 06 00 00 01"), \
        "changed GL-context active-state admission"
    dispatch_pointer = read(gl_external + 0x24, 7)
    assert dispatch_pointer[:3] == bytes.fromhex("48 03 0d") and \
        gl_external + 0x2b + struct.unpack_from("<i", dispatch_pointer, 3)[0] == \
        address_of("__ZL25sGLContextMethodsDispatch"), \
        "changed GL-context dynamic dispatch-pointer load"

    gl_vtable = address_of("__ZTV17IOAccelGLContext2")
    for slot, method in (
            (0x9e0, "__ZN17IOAccelGLContext212contextStartEv"),
            (0x9e8, "__ZN17IOAccelGLContext211contextStopEv"),
            (0xa28, "__ZN17IOAccelGLContext218processDataBuffersEj"),
            (0xb58, "__ZN17IOAccelGLContext226sleepForSwapCompleteNoLockEj"),
            (0xb60, "__ZN17IOAccelGLContext228addVendorSurfaceRequiredBitsEy"),
            (0xb70, "__ZN17IOAccelGLContext211processSwapE7eDoSwap")):
        raw = struct.unpack("<Q", read(gl_vtable + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and \
            raw & 0x3fffffff == address_of(method), \
            f"changed GL-context virtual target at {slot:#x}"

    gl_read = address_of("__ZN17IOAccelGLContext211read_bufferEP30IOAccelGLContextReadBufferData")
    assert direct_branch_offsets(
        address_of("__ZN17IOAccelGLContext213s_read_bufferEPS_PvP25IOExternalMethodArguments"),
        0xe, gl_read) == [0x9], "changed GL-context selector-0x105 read-buffer edge"
    assert direct_branch_offsets(gl_read, 0x7f6, 0x10012) == [0xea] and \
        direct_branch_offsets(gl_read, 0x7f6, 0x14ba6da2) == [0xfe] and \
        direct_branch_offsets(gl_read, 0x7f6,
                              address_of("__ZN22IOGraphicsAccelerator222acceleratorWaitEnabledEv")) == [0x144], \
        "changed GL-context read-buffer admission lock/wait sequence"
    assert direct_branch_offsets(gl_read, 0x7f6, 0x14ba6db4) == [0x1ad, 0x21b, 0x53e] and \
        direct_branch_offsets(gl_read, 0x7f6, 0x10018) == [0x1b9, 0x227, 0x54a], \
        "changed GL-context read-buffer balanced unlock inventory"
    assert read(gl_read + 0x574, 6) == bytes.fromhex("ff 90 60 09 00 00"), \
        "changed GL-context read-buffer surface copy producer dispatch"
    assert direct_branch_offsets(
        gl_read, 0x7f6,
        address_of("__ZN14IOAccelSurface21copyFromBufferWithCPUEiiiijjP16IOAccelResource2P16IOAccelSysMemoryjyj")) == [0x75b], \
        "changed GL-context read-buffer CPU fallback edge"

    gl_set_surface = address_of("__ZN17IOAccelGLContext211set_surfaceEP30IOAccelGLContextSetSurfaceData")
    assert direct_branch_offsets(gl_set_surface, 0x6d6, 0x10012) == [0x8d] and \
        direct_branch_offsets(gl_set_surface, 0x6d6, 0x14ba6da2) == [0x9d] and \
        direct_branch_offsets(gl_set_surface, 0x6d6, 0x14ba6db4) == [0x2cf, 0x64b] and \
        direct_branch_offsets(gl_set_surface, 0x6d6, 0x10018) == [0x2db, 0x657], \
        "changed GL-context set-surface lock inventory"
    assert direct_branch_offsets(
        gl_set_surface, 0x6d6,
        address_of("__ZN27IOAccelLegacyDisplayMachine16setup_fullscreenEP20IOAccelLegacySurface")) == \
        [0x592, 0x6cc], "changed GL-context legacy-display setup edges"

    gl_process = address_of("__ZN17IOAccelGLContext218processDataBuffersEj")
    gl_swap = address_of("__ZN17IOAccelGLContext211processSwapE7eDoSwap")
    assert read(gl_process + 0x676, 6) == bytes.fromhex("ff 90 70 0b 00 00"), \
        "changed GL-context process-to-swap virtual edge"
    assert read(gl_swap + 0xc8, 6) == bytes.fromhex("ff 90 70 09 00 00") and \
        read(gl_swap + 0x210, 6) == bytes.fromhex("ff 90 70 09 00 00"), \
        "changed GL-context swap surface-copy virtuals"
    assert direct_branch_offsets(
        gl_swap, 0x454, address_of("__ZN14IOAccelSurface12flip_buffersEv")) == [0x2e4, 0x342, 0x378] and \
        direct_branch_offsets(
            gl_swap, 0x454,
            address_of("__ZN20IOAccelLegacySurface25present_surface_with_flipEj")) == [0x301] and \
        direct_branch_offsets(
            gl_swap, 0x454,
            address_of("__ZN20IOAccelLegacySurface25present_surface_with_swapEjj")) == [0x3de], \
        "changed GL-context swap producer selection"

    drawable_external = address_of(
        "__ZN27IOAccelGLDrawableUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv")
    assert read(drawable_external + 4, 7) == bytes.fromhex("f6 87 08 01 00 00 01") and \
        read(drawable_external + 0x17, 3) == bytes.fromhex("83 fe 06"), \
        "changed GL-drawable active-state/selector admission"
    drawable_table = read(drawable_external + 0x21, 7)
    assert drawable_table[:3] == bytes.fromhex("4c 8d 1d") and \
        drawable_external + 0x28 + struct.unpack_from("<i", drawable_table, 3)[0] == \
        address_of("__ZN27IOAccelGLDrawableUserClient15sMethodDispatchE"), \
        "changed GL-drawable static dispatch-table selection"
    for wrapper, length, method, offsets in (
            ("__ZN27IOAccelGLDrawableUserClient13s_set_surfaceEPS_PvP25IOExternalMethodArguments", 0xe,
             "__ZN27IOAccelGLDrawableUserClient11set_surfaceEP37IOAccelGLDrawableClientSetSurfaceData", [0x9]),
            ("__ZN27IOAccelGLDrawableUserClient31s_set_surface_get_config_statusEPS_PvP25IOExternalMethodArguments", 0x12,
             "__ZN27IOAccelGLDrawableUserClient29set_surface_get_config_statusEP37IOAccelGLDrawableClientSetSurfaceDataP38IOAccelGLDrawableClientGetConfigStatusyPy", [0xd]),
            ("__ZN27IOAccelGLDrawableUserClient21s_signal_shared_eventEPS_PvP25IOExternalMethodArguments", 0x20,
             "__ZN27IOAccelGLDrawableUserClient19signal_shared_eventEP4taskyjy", [0x1a]),
            ("__ZN27IOAccelGLDrawableUserClient33s_create_mach_port_from_iosurfaceEPS_PvP25IOExternalMethodArguments", 0x16,
             "__ZN27IOAccelGLDrawableUserClient31create_mach_port_from_iosurfaceEyPj", [0x10])):
        assert direct_branch_offsets(address_of(wrapper), length, address_of(method)) == offsets, \
            f"changed GL-drawable wrapper edge: {wrapper}"
    for method, locks, unlocks, mutexes, unmutexes in (
            ("__ZN27IOAccelGLDrawableUserClient11set_surfaceEP37IOAccelGLDrawableClientSetSurfaceData",
             [0x9d], [0x27e, 0x500], [0x8d], [0x28a, 0x50c]),
            ("__ZN27IOAccelGLDrawableUserClient29set_surface_get_config_statusEP37IOAccelGLDrawableClientSetSurfaceDataP38IOAccelGLDrawableClientGetConfigStatusyPy",
             [0x61, 0x182], [0xe4, 0x345], [0x51, 0x172], [0xf0, 0x352]),
            ("__ZN27IOAccelGLDrawableUserClient19signal_shared_eventEP4taskyjy",
             [0x4e], [0x114], [0x3e], [0x120])):
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x14ba6da2) == locks and \
            direct_branch_offsets(start, length, 0x14ba6db4) == unlocks and \
            direct_branch_offsets(start, length, 0x10012) == mutexes and \
            direct_branch_offsets(start, length, 0x10018) == unmutexes, \
            f"changed GL-drawable lock inventory: {method}"
    drawable_set = address_of(
        "__ZN27IOAccelGLDrawableUserClient11set_surfaceEP37IOAccelGLDrawableClientSetSurfaceData")
    assert direct_branch_offsets(
        drawable_set, 0x516, address_of("__ZN17IOAccelSurfaceMTL12update_shapeEv")) == [0x439, 0x4c9], \
        "changed GL-drawable surface-shape edges"
    assert direct_branch_offsets(
        address_of("__ZN27IOAccelGLDrawableUserClient19signal_shared_eventEP4taskyjy"), 0x138,
        address_of("__ZN17IOAccelSurfaceMTL19signal_shared_eventEP4taskyjy")) == [0x93], \
        "changed GL-drawable shared-event registration edge"

    surface_mtl_external = address_of(
        "__ZN17IOAccelSurfaceMTL14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv")
    assert direct_branch_offsets(
        surface_mtl_external, 0x84,
        address_of("__ZN17IOAccelSurfaceMTL28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony")) == \
        [0x66], "changed SurfaceMTL selectors 6/17 shape-backing edge"
    for offset, dispatch in (
            (0x1d, "__ZZN17IOAccelSurfaceMTL14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE19newResourceDispatch"),
            (0x3c, "__ZN17IOAccelSurfaceMTL20sSignalEventDispatchE")):
        instruction = read(surface_mtl_external + offset, 7)
        assert instruction[:3] == bytes.fromhex("48 8d 0d") and \
            surface_mtl_external + offset + 7 + struct.unpack_from("<i", instruction, 3)[0] == \
            address_of(dispatch), f"changed SurfaceMTL special dispatch: {dispatch}"
    surface_mtl_vtable = address_of("__ZTV17IOAccelSurfaceMTL")
    for slot, method in (
            (0x960, "__ZN17IOAccelSurfaceMTL12surfaceStartEv"),
            (0x968, "__ZN17IOAccelSurfaceMTL11surfaceStopEv"),
            (0x970, "__ZN17IOAccelSurfaceMTL22isSurfaceSizeSupportedEss"),
            (0x978, "__ZN17IOAccelSurfaceMTL12shapeSurfaceEjtt")):
        raw = struct.unpack("<Q", read(surface_mtl_vtable + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and \
            raw & 0x3fffffff == address_of(method), \
            f"changed SurfaceMTL virtual target at {slot:#x}"
    assert read(address_of("__ZN17IOAccelSurfaceMTL12update_shapeEv") + 0x142, 6) == \
        bytes.fromhex("ff 90 78 09 00 00"), "changed SurfaceMTL update-to-shape virtual"
    shape_backing = address_of(
        "__ZN17IOAccelSurfaceMTL28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony")
    assert read(shape_backing + 0xcf, 6) == bytes.fromhex("ff 90 70 09 00 00") and \
        read(shape_backing + 0x27b, 6) == bytes.fromhex("ff 90 70 09 00 00"), \
        "changed SurfaceMTL shape-backing size validation virtuals"
    for method, locks, unlocks, mutexes, unmutexes in (
            ("__ZN17IOAccelSurfaceMTL11set_id_modeEjj", [0x81], [0x227], [0x71], [0x233]),
            ("__ZN17IOAccelSurfaceMTL9set_scaleEjP21IOAccelSurfaceScalingy", [0x56], [0xa7], [0x46], [0xb3]),
            ("__ZN17IOAccelSurfaceMTL28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony",
             [0x189], [0x2dc, 0x347], [0x179], [0x2e8, 0x353]),
            ("__ZN17IOAccelSurfaceMTL15surface_controlEjjPj", [0x48], [0x96], [0x38], [0xa2]),
            ("__ZN17IOAccelSurfaceMTL19signal_shared_eventEP4taskyjy", [0xcd], [0x222], [0xbd], [0x22e])):
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x14ba6da2) == locks and \
            direct_branch_offsets(start, length, 0x14ba6db4) == unlocks and \
            direct_branch_offsets(start, length, 0x10012) == mutexes and \
            direct_branch_offsets(start, length, 0x10018) == unmutexes, \
            f"changed SurfaceMTL lock inventory: {method}"
    assert direct_branch_offsets(
        shape_backing, 0x3a0,
        address_of("__ZN22IOGraphicsAccelerator222acceleratorWaitEnabledEv")) == [0x1b0], \
        "changed SurfaceMTL shape-backing enabled wait"
    surface_mtl_signal = address_of("__ZN17IOAccelSurfaceMTL19signal_shared_eventEP4taskyjy")
    for method, offsets in (
            ("__ZN28IOSurfaceMTLSharedEventFence15withSharedEventEP20IOSurfaceSharedEventy", [0x10c]),
            ("__ZN23IOAccelSharedEventFence15withSharedEventEP20IOSurfaceSharedEventyP22IOGraphicsAccelerator2P17IOAccelSubmitter2", [0x196]),
            ("__ZN19IOAccelFenceMachine13addEventFenceEP17IOAccelSubmitter2P17IOAccelEventFence", [0x1e6])):
        assert direct_branch_offsets(surface_mtl_signal, 0x25c, address_of(method)) == offsets, \
            f"changed SurfaceMTL shared-event fence edge: {method}"
    assert direct_branch_offsets(
        address_of("__ZN17IOAccelSurfaceMTL21s_signal_shared_eventEPS_PvP25IOExternalMethodArguments"),
        0x20, surface_mtl_signal) == [0x1a], "changed SurfaceMTL signal wrapper edge"
    print("PASS GLContext/GLDrawable/SurfaceMTL selectors, lock domains and producer classification")

    # Device selectors are read-only metadata/configuration except for the
    # API-property virtual and the unsupported stereo stub.  The three
    # collection reads that need accelerator serialization have exact busy
    # scopes; no selector is a GPU command producer.
    for method, locks, unlocks in (
            ("__ZN14IOAccelDevice210get_configEP23IOAccelDeviceConfigData", [0x40], [0x108]),
            ("__ZN14IOAccelDevice217get_event_machineEP29IOAccelDeviceEventMachineData", [0x3e], [0x129]),
            ("__ZN14IOAccelDevice216get_surface_infoEjP24IOAccelDeviceSurfaceData", [0x46], [0x1a4])):
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x14ba6da2) == locks and \
            direct_branch_offsets(start, length, 0x14ba6db4) == unlocks, \
            f"changed Device client busy-lock inventory: {method}"
    for method in (
            "__ZN14IOAccelDevice28get_nameEPc",
            "__ZN14IOAccelDevice210set_stereoEjj",
            "__ZN14IOAccelDevice225get_next_global_object_idEP31IOAccelDeviceGlobalObjectIDData",
            "__ZN14IOAccelDevice224get_current_trace_filterEP28IOAccelDeviceTraceFilterData",
            "__ZN14IOAccelDevice215get_device_infoEP27IOAccelDeviceInfoReturnData",
            "__ZN14IOAccelDevice218get_next_gid_groupEP25IOAccelDeviceGIDGroupData",
            "__ZN14IOAccelDevice216set_api_propertyEP24IOAccelDeviceAPIProperty"):
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x14ba6da2) == [] and \
            direct_branch_offsets(start, length, 0x14ba6db4) == [], \
            f"Device metadata method gained a busy-lock transition: {method}"

    shared_special = address_of(
        "__ZZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE7methods")
    assert hashlib.sha256(read(shared_special, 48)).hexdigest() == \
        "7ee90dd428e43637af578bf039d67cc4f1e20876508c5ec55e3036d53c9bf8c8", \
        "changed Shared selector-0 special dispatch record"
    entry = struct.unpack("<6Q", read(shared_special, 48))
    assert entry[0] >> 63 == 0 and (entry[0] >> 30) & 3 == 1 and \
        entry[0] & 0x3fffffff == address_of(
            "__ZN24IOAccelSharedUserClient214s_new_resourceEPS_PvP25IOExternalMethodArguments") and \
        entry[1:] == (0xffffffff00000000, 0xffffffff00000000, 0, 0, 0), \
        "changed Shared selector-0 variable-structure contract"
    shared_external = address_of(
        "__ZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv")
    selector17_instruction = read(shared_external + 0x29, 7)
    selector0_instruction = read(shared_external + 0x32, 7)
    assert selector17_instruction[:3] == bytes.fromhex("4c 8d 35") and \
        selector0_instruction[:3] == bytes.fromhex("4c 8d 35"), \
        "changed Shared selector-0/17 special dispatch instructions"
    shared_purge_special = shared_external + 0x30 + struct.unpack_from(
        "<i", selector17_instruction, 3)[0]
    assert shared_external + 0x39 + struct.unpack_from("<i", selector0_instruction, 3)[0] == \
        shared_special, "changed Shared selector-0 special dispatch selection"
    assert hashlib.sha256(read(shared_purge_special, 48)).hexdigest() == \
        "e97d73096e047fad99684bf05805d4c1042f78b7a166832b6d7ca8e6a7a00446", \
        "changed Shared selector-17 special dispatch record"
    purge_entry = struct.unpack("<Q4I", read(shared_purge_special, 24))
    assert purge_entry[0] >> 63 == 0 and purge_entry[0] & 0x3fffffff == address_of(
        "__ZN24IOAccelSharedUserClient225s_set_resources_purgeableEPS_PvP25IOExternalMethodArguments") and \
        purge_entry[1:] == (1, 0xffffffff, 0, 0xffffffff), \
        "changed Shared selector-17 variable-array contract"
    assert direct_branch_offsets(
        address_of("__ZN24IOAccelSharedUserClient214s_new_resourceEPS_PvP25IOExternalMethodArguments"),
        0x12c,
        address_of("__ZN24IOAccelSharedUserClient212new_resourceEP22IOAccelNewResourceArgsP28IOAccelNewResourceReturnDatayPj")) == [0xa2], \
        "changed Shared selector-0 wrapper-to-member edge"

    shared_lock_inventory = (
        ("__ZN24IOAccelSharedUserClient212new_resourceEP22IOAccelNewResourceArgsP28IOAccelNewResourceReturnDatayPj", [0x1d7], [0x34c, 0xa15]),
        ("__ZN24IOAccelSharedUserClient215delete_resourceEj", [0x54], [0xf3]),
        ("__ZN24IOAccelSharedUserClient217page_off_resourceEP32IOAccelSharedPageoffResourceArgs", [0x54], [0x9d, 0x110, 0x1e8]),
        ("__ZN24IOAccelSharedUserClient219finish_object_eventEjj", [0x87, 0x176], [0x125, 0x1de]),
        ("__ZN24IOAccelSharedUserClient222set_resource_purgeableEj25eIOAccelResourcePurgeablePS0_", [0x76], [0x13b]),
        ("__ZN24IOAccelSharedUserClient216get_surface_infoEjPjS0_S0_S0_S0_", [0x53], [0x1b2]),
        ("__ZN24IOAccelSharedUserClient217get_resource_infoEjP32IOAccelGetResourceInfoReturnDataPj", [0x6b], [0xe9]),
        ("__ZN24IOAccelSharedUserClient212create_shmemEjP22IOAccelDeviceShmemData", [0x46], [0x9c]),
        ("__ZN24IOAccelSharedUserClient213destroy_shmemEj", [0x3e], [0x88]),
        ("__ZN24IOAccelSharedUserClient215get_shared_infoEP30IOAccelSharedGetInfoReturnData", [0x44], [0xc3]),
        ("__ZN24IOAccelSharedUserClient216setup_dirty_ringEP37IOAccelSharedSetupDirtyRingReturnData", [0x3e], [0x90]),
        ("__ZN24IOAccelSharedUserClient222process_dirty_commandsEv", [0x3a], [0x7e]),
        ("__ZN24IOAccelSharedUserClient221allocate_fence_memoryEPyS0_", [0x46], [0x94]),
        ("__ZN24IOAccelSharedUserClient215create_mtleventEPyP27IOAccelCreateMTLEventResult", [0x46], [0x94]),
        ("__ZN24IOAccelSharedUserClient216destroy_mtleventEj", [0x3e], [0x88]),
        ("__ZN24IOAccelSharedUserClient215get_memory_dataEP17IOAccelMemoryData", [0x3e], [0x85]),
        ("__ZN24IOAccelSharedUserClient215disconnect_peerEj", [0x3e], [0x88]),
        ("__ZN24IOAccelSharedUserClient223set_resources_purgeableEPKj25eIOAccelResourcePurgeablePS2_i", [], []),
        ("__ZN24IOAccelSharedUserClient219get_resource_offsetEPyS0_", [0x63], [0xf3]),
        ("__ZN24IOAccelSharedUserClient218get_allocated_sizeEP20IOAccelAllocatedSize", [0x3e], [0x8a]),
        ("__ZN24IOAccelSharedUserClient227set_resource_owner_identityEP43IOAccelResourceSetResourceOwnerIdentityData", [0x47], [0x95]),
    )
    for method, locks, unlocks in shared_lock_inventory:
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x14ba6da2) == locks and \
            direct_branch_offsets(start, length, 0x14ba6db4) == unlocks, \
            f"changed Shared selector busy-lock inventory: {method}"
    shared_new = address_of(
        "__ZN24IOAccelSharedUserClient212new_resourceEP22IOAccelNewResourceArgsP28IOAccelNewResourceReturnDatayPj")
    assert direct_branch_offsets(
        shared_new, 0xc6a,
        address_of("__ZN22IOGraphicsAccelerator222acceleratorWaitEnabledEv")) == [0x1f8], \
        "changed Shared new-resource enabled wait"
    shared_pageoff = address_of(
        "__ZN24IOAccelSharedUserClient217page_off_resourceEP32IOAccelSharedPageoffResourceArgs")
    assert read(shared_pageoff + 0x155, 6) == bytes.fromhex("ff 90 60 02 00 00"), \
        "changed Shared page-off to resource pageoffIfNeeded dispatch"
    resource_vtable = address_of("__ZTV16IOAccelResource2")
    for slot, method in (
            (0x170, "__ZN16IOAccelResource27prepareEv"),
            (0x178, "__ZN16IOAccelResource28completeEv"),
            (0x180, "__ZN16IOAccelResource24loadEv"),
            (0x188, "__ZN16IOAccelResource26unloadEv"),
            (0x260, "__ZN16IOAccelResource215pageoffIfNeededEjj"),
            (0x268, "__ZN16IOAccelResource214pageonIfNeededEv"),
            (0x270, "__ZN16IOAccelResource29gartEventEv")):
        raw = struct.unpack("<Q", read(resource_vtable + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and raw & 0x3fffffff == address_of(method), \
            f"changed resource paging virtual at {slot:#x}"
    resource_pageoff = address_of("__ZN16IOAccelResource215pageoffIfNeededEjj")
    assert read(resource_pageoff + 0x421, 6) == bytes.fromhex("ff 90 c8 01 00 00"), \
        "changed resource pageoffIfNeeded concrete page-off dispatch"

    # Pin the inherited transitive bridge into the concrete Intel paging
    # methods.  These edges deliberately remain below admission: native event
    # retirement and teardown use the same unload/page-off chain.
    paging_edges = (
        ("__ZN16IOAccelResource27prepareEv", 0x188, 0x180),
        ("__ZN16IOAccelResource24loadEv", 0x1f, 0x268),
        ("__ZN16IOAccelResource26unloadEv", 0x40, 0x260),
        ("__ZN16IOAccelResource27unpurgeEb", 0x113, 0x260),
        ("__ZN16IOAccelResource215pageoffInLinearEv", 0xa3, 0x268),
        ("__ZN16IOAccelResource215pageoffInLinearEv", 0xf9, 0x260),
        ("__ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi", 0x23a, 0x268),
        ("__ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi", 0x395, 0x260),
        ("__ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi", 0x42c, 0x188),
        ("__ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi", 0x4a4, 0x188),
        ("__ZN16IOAccelResource217getPhysicalOffsetEyPy", 0x2c, 0x170),
    )
    for method, offset, slot in paging_edges:
        assert read(address_of(method) + offset, 6) == \
            bytes.fromhex("ff 90") + struct.pack("<I", slot), \
            f"changed inherited resource paging edge: {method}+{offset:#x}"
    assert direct_branch_offsets(
        address_of("__ZN24IOAccelSharedUserClient219get_resource_offsetEPyS0_"),
        BASE_CLIENT_BODIES["__ZN24IOAccelSharedUserClient219get_resource_offsetEPyS0_"][0],
        address_of("__ZN16IOAccelResource217getPhysicalOffsetEyPy")) == [0xc1], \
        "changed Shared physical-offset to resource-prepare path"
    assert read(address_of(
        "__ZN24IOAccelSharedUserClient212new_resourceEP22IOAccelNewResourceArgsP28IOAccelNewResourceReturnDatayPj") +
        0xa76, 6) == bytes.fromhex("ff 90 88 01 00 00"), \
        "changed Shared new-resource cleanup unload path"
    resource_cpu_lock = address_of(
        "__ZN16IOAccelResource216lockForCPUAccessEP4task9eLockTypejbhhPi")
    for method, offset in (
            ("__ZN14IOAccelSurface20surface_lock_optionsE9eLockTypejP25IOAccelSurfaceInformationy", 0x318),
            ("__ZN20IOAccelLegacySurface20surface_lock_optionsE9eLockTypejP25IOAccelSurfaceInformationy", 0x342)):
        assert direct_branch_offsets(
            address_of(method), BASE_CLIENT_BODIES.get(
                method, RESOURCE_PAGING_BODIES.get(method))[0], resource_cpu_lock) == [offset], \
            f"changed Surface CPU-lock paging path: {method}"
    assert direct_branch_offsets(
        address_of("__ZN22IOGraphicsAccelerator222pageoffSurfaceInLinearEv"), 0x44,
        address_of("__ZN14IOAccelSurface15pageoffInLinearEv")) == [0x36] and \
        direct_branch_offsets(
            address_of("__ZN14IOAccelSurface15pageoffInLinearEv"), 0x4e,
            address_of("__ZN16IOAccelResource215pageoffInLinearEv")) == [0x46], \
        "changed accelerator-to-surface linear page-off chain"
    assert direct_branch_offsets(
        address_of("__ZL23IOAcceleratorKDCallbackPv16kd_callback_typeS_"), 0x261,
        address_of("__ZN22IOGraphicsAccelerator220emitFirstFlushEventsEv")) == [0x214], \
        "changed KD first-flush resource-prepare root"
    kd_callback = address_of("__ZL23IOAcceleratorKDCallbackPv16kd_callback_typeS_")
    assert kd_callback == 0x14ba774f, "changed KD callback address"
    assert direct_branch_offsets(kd_callback, 0x261, 0x10282) == [0x17c] and \
        direct_branch_offsets(kd_callback, 0x261, 0x10288) == [0x190] and \
        direct_branch_offsets(kd_callback, 0x261, 0x1007e) == [0x1c8], \
        "changed KD matching-services iterator/dynamic-cast calls"
    assert read(kd_callback + 0x198, 9) == bytes.fromhex(
        "49 8b 06 4c 89 f7 ff 50 28"), \
        "changed KD matching dictionary release after iterator creation"
    assert read(kd_callback + 0x1aa, 12) == bytes.fromhex(
        "48 8b 03 48 89 df ff 90 28 01 00 00") and \
        read(kd_callback + 0x219, 12) == bytes.fromhex(
            "48 8b 03 48 89 df ff 90 28 01 00 00"), \
        "changed KD iterator getNextObject loop"
    assert read(kd_callback + 0x22a, 0x17) == bytes.fromhex(
        "48 8b 03 48 89 df 48 83 c4 08 5b 41 5c 41 5d 41 5e 41 5f 5d ff 60 28"), \
        "changed KD iterator terminal release"
    kd_register = read(0x14ba1236, 5)
    assert kd_register[0] == 0xe8 and \
        0x14ba123b + struct.unpack_from("<i", kd_register, 1)[0] == 0x11272, \
        "changed permanent KD callback registration edge"
    assert direct_branch_offsets(
        address_of("__ZN22IOGraphicsAccelerator214gart_collectorEP22IOInterruptEventSourcei"),
        EVENT_OWNER_BODIES["__ZN22IOGraphicsAccelerator214gart_collectorEP22IOInterruptEventSourcei"][0],
        address_of("__ZN22IOGraphicsAccelerator226try_unload_dirty_resourcesEj")) == [0xe4], \
        "changed gart-collector transitive page-off root"
    assert direct_branch_offsets(
        address_of("__ZN22IOGraphicsAccelerator218unwireAllVidMemoryEv"),
        RESOURCE_PAGING_BODIES["__ZN22IOGraphicsAccelerator218unwireAllVidMemoryEv"][0],
        address_of("__ZN22IOGraphicsAccelerator222unload_dirty_resourcesEv")) == [0xe], \
        "changed unwire-to-resource-unload path"

    # Four callback/control roots sit outside the user-client inventory:
    # display notifications, the GART collector, IOSurface device-cache
    # control, and the global KD first-flush callback.  Pin their registration
    # sites and concrete receiver ownership.  Lower resource methods remain
    # shared with the retirement paths partitioned above.
    for lea, target in (
            (0x14ba037e, "__ZN22IOGraphicsAccelerator214gart_collectorEP22IOInterruptEventSourcei"),
            (0x14ba1201, "__ZL23IOAcceleratorKDCallbackPv16kd_callback_typeS_"),
            (0x14ba3caf, "__ZN22IOGraphicsAccelerator218deviceCacheControlEP20IOSurfaceDeviceCachejyy"),
            (0x14bae504, "__ZN18IOAccelDisplayPipe22display_change_handlerEPvP13IOFramebufferiS0_")):
        encoded = read(lea, 7)
        assert encoded[0] in (0x48, 0x4c) and encoded[1] == 0x8d and \
            encoded[2] & 0xc7 == 0x05 and \
            lea + 7 + struct.unpack_from("<i", encoded, 3)[0] == address_of(target), \
            f"changed paging-control callback registration: {target}"

    # GART collection is workloop-owned.  Registration stores the event
    # source at accelerator+0x110, adds it to accelerator+0xf0's workloop and
    # enables it.  The inherited stop block runs under that workloop gate;
    # IOWorkLoop::removeEventSource is synchronous and is followed by the
    # source release and field clear.  Pin both ends so the VF producer drain
    # cannot be invalidated by an untracked surviving event source.
    assert read(0x14ba0391, 0x2f) == bytes.fromhex(
        "49 89 86 10 01 00 00 48 85 c0 74 23 "
        "49 8b be f0 00 00 00 48 8b 0f 48 89 c6 "
        "ff 91 40 01 00 00 49 8b be 10 01 00 00 "
        "48 8b 07 ff 90 50 01 00 00"), \
        "changed GART event-source store/add/enable sequence"
    assert read(0x14ba20af, 0x34) == bytes.fromhex(
        "48 8b b3 10 01 00 00 48 85 f6 74 28 "
        "48 8b bb f0 00 00 00 48 8b 07 ff 90 48 01 00 00 "
        "48 8b bb 10 01 00 00 48 8b 07 ff 50 28 "
        "48 c7 83 10 01 00 00 00 00 00 00"), \
        "changed GART event-source synchronous remove/release/clear sequence"

    # Finalization is not an ordinary external producer.  Both finalize entry
    # points signal a dedicated interrupt source at accelerator+0x9e0.  Its
    # handler synchronously asks IOSurfaceRoot to retire every cache for this
    # accelerator, then invokes acceleratorFinalize under the native lock.
    # Stop removes/releases/clears this source through the same synchronous
    # workloop-maintenance path used above.  The paired BootKC check below pins
    # the imported IOSurface target and selector-3/4 callback semantics.
    finalize_interrupt = address_of(
        "__ZN22IOGraphicsAccelerator218finalize_interruptEP22IOInterruptEventSourcei")
    assert finalize_interrupt == 0x14ba1766, "changed accelerator finalize handler address"
    finalize_lea = read(0x14ba0534, 7)
    assert finalize_lea[:3] == bytes.fromhex("48 8d 35") and \
        0x14ba053b + struct.unpack_from("<i", finalize_lea, 3)[0] == finalize_interrupt, \
        "changed accelerator finalize callback registration"
    assert read(0x14ba0547, 0x2f) == bytes.fromhex(
        "49 89 86 e0 09 00 00 48 85 c0 74 23 "
        "49 8b be f0 00 00 00 48 8b 0f 48 89 c6 "
        "ff 91 40 01 00 00 49 8b be e0 09 00 00 "
        "48 8b 07 ff 90 50 01 00 00"), \
        "changed accelerator finalize-source store/add/enable sequence"
    assert read(0x14ba1ec5, 0x44) == bytes.fromhex(
        "48 8b bb e0 09 00 00 48 85 ff 74 38 48 8b 07 "
        "ff 90 58 01 00 00 48 8b bb f0 00 00 00 "
        "48 8b b3 e0 09 00 00 48 8b 07 ff 90 48 01 00 00 "
        "48 8b bb e0 09 00 00 48 8b 07 ff 50 28 "
        "48 c7 83 e0 09 00 00 00 00 00 00"), \
        "changed accelerator finalize-source synchronous remove/release/clear sequence"
    assert direct_branch_offsets(finalize_interrupt, 0x116, 0x111ca) == [0x2b], \
        "changed accelerator-finalize to IOSurface cache-termination edge"
    accelerator_finalize = "__ZN22IOGraphicsAccelerator219acceleratorFinalizeEv"
    raw_finalize = struct.unpack(
        "<Q", read(address_of("__ZTV22IOGraphicsAccelerator2") + 16 + 0x9a0, 8))[0]
    assert raw_finalize >> 63 == 0 and (raw_finalize >> 30) & 3 == 1 and \
        raw_finalize & 0x3fffffff == address_of(accelerator_finalize), \
        "changed acceleratorFinalize virtual target"
    assert read(finalize_interrupt + 0x7d, 6) == bytes.fromhex("ff 90 a0 09 00 00"), \
        "changed finalize handler acceleratorFinalize dispatch"
    finalize = address_of("__ZN22IOGraphicsAccelerator28finalizeEj")
    finalize_if_possible = address_of(
        "__ZN22IOGraphicsAccelerator220finalize_if_possibleEv")
    assert read(finalize + 0x3d, 0x16) == bytes.fromhex(
        "48 8b bb e0 09 00 00 48 8b 07 31 f6 31 d2 31 c9 "
        "ff 90 d8 01 00 00"), \
        "changed finalize event-source signal"
    assert read(finalize_if_possible + 0x22, 0x1a) == bytes.fromhex(
        "48 8b bf e0 09 00 00 48 8b 07 48 8b 80 d8 01 00 00 "
        "31 f6 31 d2 31 c9 5d ff e0"), \
        "changed finalize-if-possible event-source signal"

    # IOService's display-interest notifier is retained by the pipe at +0xa0.
    # IONotifier::remove is synchronizing with handler execution by API
    # contract; IOAccelDisplayPipe::free invokes it before clearing the field
    # or releasing any other pipe state.  This is the lifetime proof required
    # for the display wrapper's pre-lease pipe->accelerator load.
    assert read(0x14bae504, 0x2e) == bytes.fromhex(
        "48 8d 35 79 01 00 00 4c 89 e7 48 89 da 4c 89 f9 "
        "41 b8 70 01 00 00 45 31 c9 e8 28 52 15 00 "
        "48 89 83 a0 00 00 00 48 85 c0 0f 84 20 01 00 00"), \
        "changed display notification registration/store/null-check sequence"
    assert read(0x14baeab6, 0x20) == bytes.fromhex(
        "48 8b bb a0 00 00 00 48 85 ff 74 14 48 8b 07 "
        "ff 90 18 01 00 00 48 c7 83 a0 00 00 00 00 00 00 00"), \
        "changed display notifier synchronous remove/clear sequence"
    memory_info_start = address_of("__ZN27IOAccelMemoryInfoUserClient5startEP9IOService")
    assert read(memory_info_start + 0x26, 7) == bytes.fromhex("49 89 86 e0 00 00 00"), \
        "changed MemoryInfo accelerator ownership"
    display_handler = address_of(
        "__ZN18IOAccelDisplayPipe22display_change_handlerEPvP13IOFramebufferiS0_")
    assert read(display_handler + 0x63, 7) == bytes.fromhex("4d 8b be 88 00 00 00") and \
        read(display_handler + 0x12f, 7) == bytes.fromhex("49 8b be 90 00 00 00"), \
        "changed display callback accelerator/display-machine ownership"
    device_cache_callback = address_of(
        "__ZN22IOGraphicsAccelerator218deviceCacheControlEP20IOSurfaceDeviceCachejyy")
    assert read(device_cache_callback + 0x20, 3) == bytes.fromhex("48 89 fb"), \
        "changed device-cache accelerator receiver"
    cache_jump_table = 0x14ba422c
    cache_targets = tuple(
        cache_jump_table + struct.unpack("<i", read(cache_jump_table + selector * 4, 4))[0]
        for selector in range(10))
    assert cache_targets[3] == 0x14ba3e4d and cache_targets[4] == 0x14ba4021, \
        "changed device-cache selector-3/4 retirement dispatch"
    selector3_release = read(0x14ba3ea6, 11)
    assert selector3_release[:6] == bytes.fromhex("49 8b 07 4c 89 ff") and \
        selector3_release[6] == 0xe9 and \
        0x14ba3eb1 + struct.unpack_from("<i", selector3_release, 7)[0] == 0x14ba41ef and \
        read(0x14ba41ef, 3) == bytes.fromhex("ff 50 28"), \
        "changed selector-3 final cache release edge"
    assert read(0x14ba4021, 7) == bytes.fromhex("4d 8b 77 20 4c 89 f7") and \
        read(0x14ba4081, 0x22) == bytes.fromhex(
            "49 c7 86 e0 00 00 00 00 00 00 00 49 8b 06 4c 89 f7 "
            "48 83 c4 08 5b 41 5c 41 5d 41 5e 41 5f 5d ff 60 28"), \
        "changed selector-4 resource clear/release edge"

    # Receiver-to-accelerator fields used by the counted outer admission
    # wrappers.  Each instruction is inside a separately full-body-hashed
    # producer owner above; spelling the loads out here prevents a future
    # payload from silently turning an object-layout assumption into a route.
    for method, offset, encoded in (
            ("__ZN19IOAccelCommandQueue22submit_command_buffersEPK29IOAccelCommandQueueSubmitArgs",
             0x17, "4c 8b b7 c0 05 00 00"),
            ("__ZN15IOAccelContext219submit_data_buffersEP33IOAccelContextSubmitDataBuffersInP34IOAccelContextSubmitDataBuffersOutyPy",
             0x71, "4c 8b 83 a8 05 00 00"),
            ("__ZN17IOAccel2DContext211set_surfaceEj23eIOAccelContextModeBits",
             0x52, "4c 8b af a8 05 00 00"),
            ("__ZN17IOAccel2DContext26finishEj",
             0x2d, "4c 8b bb a8 05 00 00"),
            ("__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy",
             0x4d, "4c 8b a3 a8 05 00 00"),
            ("__ZN14IOAccelSurface12surface_readEP22IOAccelSurfaceReadDatay",
             0x53, "4c 8b b7 c8 12 00 00"),
            ("__ZN24IOAccelSharedUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv",
             0xe3, "4c 8b bb f8 00 00 00"),
            ("__ZN17IOAccelGLContext211read_bufferEP30IOAccelGLContextReadBufferData",
             0xc9, "4c 8b af a8 05 00 00"),
            ("__ZN27IOAccelGLDrawableUserClient11set_surfaceEP37IOAccelGLDrawableClientSetSurfaceData",
             0x70, "4c 8b b7 f8 00 00 00"),
            ("__ZN17IOAccelSurfaceMTL28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony",
             0x15a, "4c 8b a3 f8 02 00 00"),
            ("__ZN27IOAccelMemoryInfoUserClient20purge_all_vid_memoryEv",
             0x1e, "48 8b bb e0 00 00 00"),
            ("__ZN29IOAccelDisplayPipeUserClient211copySurfaceEjj",
             0x17, "4c 8b a7 d8 00 00 00")):
        expected = bytes.fromhex(encoded)
        assert read(address_of(method) + offset, len(expected)) == expected, \
            f"changed external-producer accelerator owner: {method}"
    print("PASS all counted outer-route receiver-to-accelerator fields")

    for table, slot, method in (
            ("__ZTV21IOAccelDisplayMachine", 0x8b8,
             "__ZN21IOAccelDisplayMachine26framebuffer_will_power_offEj"),
            ("__ZTV21IOAccelDisplayMachine", 0x8c0,
             "__ZN21IOAccelDisplayMachine24framebuffer_did_power_onEj"),
            ("__ZTV21IOAccelDisplayMachine", 0x8c8,
             "__ZN21IOAccelDisplayMachine24display_mode_will_changeEj"),
            ("__ZTV21IOAccelDisplayMachine", 0x8d0,
             "__ZN21IOAccelDisplayMachine23display_mode_did_changeEj"),
            ("__ZTV27IOAccelLegacyDisplayMachine", 0x8b8,
             "__ZN27IOAccelLegacyDisplayMachine26framebuffer_will_power_offEj"),
            ("__ZTV27IOAccelLegacyDisplayMachine", 0x8c0,
             "__ZN27IOAccelLegacyDisplayMachine24framebuffer_did_power_onEj"),
            ("__ZTV27IOAccelLegacyDisplayMachine", 0x8c8,
             "__ZN27IOAccelLegacyDisplayMachine24display_mode_will_changeEj"),
            ("__ZTV27IOAccelLegacyDisplayMachine", 0x8d0,
             "__ZN27IOAccelLegacyDisplayMachine23display_mode_did_changeEj"),
            ("__ZTV18IOAccelDisplayPipe", 0x868,
             "__ZN18IOAccelDisplayPipe21displayModeWillChangeEv"),
            ("__ZTV18IOAccelDisplayPipe", 0x870,
             "__ZN18IOAccelDisplayPipe20displayModeDidChangeEv"),
            ("__ZTV18IOAccelDisplayPipe", 0x8f8,
             "__ZN18IOAccelDisplayPipe17wsaaWillExitDeferEi"),
            ("__ZTV18IOAccelDisplayPipe", 0x908,
             "__ZN18IOAccelDisplayPipe18wsaaWillEnterDeferEi"),
            ("__ZTV18IOAccelDisplayPipe", 0x978,
             "__ZN18IOAccelDisplayPipe26framebuffer_will_power_offEv"),
            ("__ZTV18IOAccelDisplayPipe", 0x980,
             "__ZN18IOAccelDisplayPipe24framebuffer_did_power_onEv"),
            ("__ZTV24IOAccelLegacyDisplayPipe", 0x868,
             "__ZN24IOAccelLegacyDisplayPipe21displayModeWillChangeEv"),
            ("__ZTV24IOAccelLegacyDisplayPipe", 0x870,
             "__ZN24IOAccelLegacyDisplayPipe20displayModeDidChangeEv"),
            ("__ZTV24IOAccelLegacyDisplayPipe", 0x978,
             "__ZN24IOAccelLegacyDisplayPipe26framebuffer_will_power_offEv"),
            ("__ZTV24IOAccelLegacyDisplayPipe", 0x980,
             "__ZN24IOAccelLegacyDisplayPipe24framebuffer_did_power_onEv")):
        raw = struct.unpack("<Q", read(address_of(table) + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and \
            raw & 0x3fffffff == address_of(method), \
            f"changed display-control virtual: {table} {slot:#x}"

    for call, slot in (
            (0x14bae7c0, 0x8c8), (0x14bae89a, 0x8b8),
            (0x14bae907, 0x8d0), (0x14bae964, 0x8c0),
            (0x14b734e0, 0x908), (0x14b73670, 0x8f8),
            (0x14b737ed, 0x978), (0x14b73893, 0x980)):
        assert read(call, 6) == bytes.fromhex("ff 90") + struct.pack("<I", slot), \
            f"changed display callback/control dispatch at {call:#x}"
    for machine, unload_offset, linear_offset, unwire_offset in (
            ("__ZN21IOAccelDisplayMachine24display_mode_will_changeEj", 0x103, 0x11e, 0x12e),
            ("__ZN27IOAccelLegacyDisplayMachine24display_mode_will_changeEj", 0x1f4, 0x20f, 0x21f)):
        start = address_of(machine)
        assert direct_branch_offsets(
            start, RESOURCE_PAGING_BODIES[machine][0],
            address_of("__ZN22IOGraphicsAccelerator222unload_dirty_resourcesEv")) == [unload_offset] and \
            read(start + linear_offset, 6) == bytes.fromhex("ff 90 58 09 00 00") and \
            read(start + unwire_offset, 6) == bytes.fromhex("ff 90 48 09 00 00"), \
            f"changed display-mode paging-control chain: {machine}"
    for owner, target, expected in (
            ("__ZN24IOAccelLegacyDisplayPipe26framebuffer_will_power_offEv",
             "__ZN24IOAccelLegacyDisplayPipe20save_scanout_surfaceEv", [0x9]),
            ("__ZN24IOAccelLegacyDisplayPipe26framebuffer_will_power_offEv",
             "__ZN24IOAccelLegacyDisplayPipe23save_fullscreen_surfaceEv", [0x1a]),
            ("__ZN24IOAccelLegacyDisplayPipe24framebuffer_did_power_onEv",
             "__ZN24IOAccelLegacyDisplayPipe26restore_fullscreen_surfaceEv", [0x4c]),
            ("__ZN18IOAccelDisplayPipe22display_change_handlerEPvP13IOFramebufferiS0_",
             "__ZN22IOGraphicsAccelerator217system_will_sleepEib", [0x1e5]),
            ("__ZN22IOGraphicsAccelerator215systemWillSleepEv",
             "__ZN21IOAccelDisplayMachine17system_will_sleepEv", [0x1c])):
        assert direct_branch_offsets(
            address_of(owner), RESOURCE_PAGING_BODIES.get(
                owner, EVENT_OWNER_BODIES.get(owner))[0], address_of(target)) == expected, \
            f"changed outer paging-control edge: {owner}"
    print("PASS inherited resource roots, four control callbacks and shared retirement bridge")

    memory_external = address_of(
        "__ZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv")
    assert read(memory_external + 4, 5) == bytes.fromhex("83 fe 02 76 07"), \
        "changed MemoryInfo three-selector bound"
    memory_table = read(memory_external + 0x16, 7)
    assert memory_table[:3] == bytes.fromhex("48 8d 0d") and \
        memory_external + 0x1d + struct.unpack_from("<i", memory_table, 3)[0] == address_of(
            "__ZZN27IOAccelMemoryInfoUserClient14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE15gather_dispatch"), \
        "changed MemoryInfo dispatch-table selection"
    for wrapper, member, offsets in (
            ("__ZN27IOAccelMemoryInfoUserClient20s_gather_memory_dataEP8OSObjectPvP25IOExternalMethodArguments",
             "__ZN27IOAccelMemoryInfoUserClient18gather_memory_dataEjPjS0_Pv", [0x94]),
            ("__ZN27IOAccelMemoryInfoUserClient27s_gather_memory_data_totalsEP8OSObjectPvP25IOExternalMethodArguments",
             "__ZN27IOAccelMemoryInfoUserClient25gather_memory_data_totalsEP12OSDictionaryPjP28IOAccelMemoryInfoAllocTotals", [0x4a]),
            ("__ZN27IOAccelMemoryInfoUserClient22s_purge_all_vid_memoryEP8OSObjectPvP25IOExternalMethodArguments",
             "__ZN27IOAccelMemoryInfoUserClient20purge_all_vid_memoryEv", [0x3d])):
        assert direct_branch_offsets(
            address_of(wrapper), BASE_CLIENT_BODIES[wrapper][0], address_of(member)) == offsets, \
            f"changed MemoryInfo wrapper edge: {wrapper}"
    memory_lock = address_of("__ZN27IOAccelMemoryInfoUserClient17lock_with_timeoutEy")
    assert direct_branch_offsets(memory_lock, 0xae, 0x14ba6da2) == [0x79] and \
        read(memory_lock + 0x19, 7) == bytes.fromhex("4d 69 e6 40 42 0f 00"), \
        "changed MemoryInfo bounded lock/busy admission"
    for method, lock_edge, unlock_edge in (
            ("__ZN27IOAccelMemoryInfoUserClient18gather_memory_dataEjPjS0_Pv", 0x2c, 0x70),
            ("__ZN27IOAccelMemoryInfoUserClient25gather_memory_data_totalsEP12OSDictionaryPjP28IOAccelMemoryInfoAllocTotals", 0x1f, 0x62),
            ("__ZN27IOAccelMemoryInfoUserClient20purge_all_vid_memoryEv", 0xf, 0x60)):
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, memory_lock) == [lock_edge] and \
            direct_branch_offsets(start, length, 0x14ba6db4) == [unlock_edge], \
            f"changed MemoryInfo lock/unlock scope: {method}"
    memory_purge = address_of("__ZN27IOAccelMemoryInfoUserClient20purge_all_vid_memoryEv")
    assert read(memory_purge + 0x28, 6) == bytes.fromhex("ff 90 48 09 00 00") and \
        read(memory_purge + 0x38, 6) == bytes.fromhex("ff 90 78 09 00 00"), \
        "changed MemoryInfo video/system unwire dispatches"
    print("PASS Device/Shared/MemoryInfo selectors, lock scopes and page-off producer root")

    # The display user client is a legacy 24-byte dispatch-table owner.  Pin
    # every wrapper edge, not just transaction-end, because selector 12 is a
    # second producer: copySurface reaches the accelerator submitSwapCopy
    # virtual at +0x9a8.
    display_external = address_of(
        "__ZN29IOAccelDisplayPipeUserClient214externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv")
    assert read(display_external + 4, 3) == bytes.fromhex("83 fe 0e"), \
        "changed DisplayPipeUserClient fourteen-selector bound"
    display_table_instruction = read(display_external + 0xd, 7)
    assert display_table_instruction[:3] == bytes.fromhex("4c 8d 15") and \
        display_external + 0x14 + struct.unpack_from("<i", display_table_instruction, 3)[0] == address_of(
            "__ZN29IOAccelDisplayPipeUserClient219sDisplayMethodDescsE") and \
        read(display_external + 0x20, 14) == bytes.fromhex(
            "48 8b 05 73 97 01 00 5d ff a0 60 08 00 00"), \
        "changed DisplayPipeUserClient table selection/inherited dispatch"
    display_wrapper_edges = (
        ("__ZN29IOAccelDisplayPipeUserClient216s_set_pipe_indexEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient212setPipeIndexEjPy", [0xf]),
        ("__ZN29IOAccelDisplayPipeUserClient225s_get_display_mode_scalerEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient220getDisplayModeScalerEP24IOAccelDisplayPipeScaler", [0x9]),
        ("__ZN29IOAccelDisplayPipeUserClient223s_get_capabilities_dataEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient219getCapabilitiesDataEPhPy", [0x70]),
        ("__ZN29IOAccelDisplayPipeUserClient216s_request_notifyEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient213requestNotifyEPyP35IOAccelDisplayPipeRequestNotifyArgs", [0x18]),
        ("__ZN29IOAccelDisplayPipeUserClient219s_transaction_beginEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient216transactionBeginEPj", [0x1c]),
        ("__ZN29IOAccelDisplayPipeUserClient235s_transaction_set_plane_gamma_tableEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient229transactionSetPlaneGammaTableEP32IOAccelDisplayPipeGammaTableArgs", [0x8a]),
        ("__ZN29IOAccelDisplayPipeUserClient237s_transaction_set_pipe_pregamma_tableEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient231transactionSetPipePreGammaTableEP32IOAccelDisplayPipeGammaTableArgs", [0x8a]),
        ("__ZN29IOAccelDisplayPipeUserClient238s_transaction_set_pipe_postgamma_tableEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient232transactionSetPipePostGammaTableEP32IOAccelDisplayPipeGammaTableArgs", [0x8a]),
        ("__ZN29IOAccelDisplayPipeUserClient217s_transaction_endEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs", [0x9]),
        ("__ZN29IOAccelDisplayPipeUserClient218s_transaction_waitEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient215transactionWaitEP37IOAccelDisplayPipeTransactionWaitArgs", [0x9]),
        ("__ZN29IOAccelDisplayPipeUserClient246s_transaction_set_pipe_precsclinearization_vidEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient240transactionSetPipePreCSCLinearizationVIDEP41IOAccelDisplayPipePreCSCLinearizationArgs", [0x86]),
        ("__ZN29IOAccelDisplayPipeUserClient239s_transaction_set_pipe_postcscgamma_vidEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient233transactionSetPipePostCSCGammaVIDEP39IOAccelDisplayPipePostCSCGammaTableArgs", [0x86]),
        ("__ZN29IOAccelDisplayPipeUserClient214s_copy_surfaceEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient211copySurfaceEjj", [0x2d]),
        ("__ZN29IOAccelDisplayPipeUserClient28s_triageEPS_PvP25IOExternalMethodArguments",
         "__ZN29IOAccelDisplayPipeUserClient26triageEPPcPy", [0x78]),
    )
    for wrapper, member, offsets in display_wrapper_edges:
        body = BASE_CLIENT_BODIES.get(wrapper, EVENT_OWNER_BODIES.get(wrapper))
        assert body is not None and direct_branch_offsets(
            address_of(wrapper), body[0], address_of(member)) == offsets, \
            f"changed DisplayPipeUserClient wrapper edge: {wrapper}"

    display_lock_inventory = (
        ("__ZN29IOAccelDisplayPipeUserClient212setPipeIndexEjPy", [0x46], [0x10d], [0x36], [0x119]),
        ("__ZN29IOAccelDisplayPipeUserClient220getDisplayModeScalerEP24IOAccelDisplayPipeScaler", [0x3e], [0x9a], [0x2e], [0xa6]),
        ("__ZN29IOAccelDisplayPipeUserClient219getCapabilitiesDataEPhPy", [0x45], [0x131], [0x35], [0x13d]),
        ("__ZN29IOAccelDisplayPipeUserClient213requestNotifyEPyP35IOAccelDisplayPipeRequestNotifyArgs", [0x60], [0xbc], [0x50], [0xc8]),
        ("__ZN29IOAccelDisplayPipeUserClient216transactionBeginEPj", [0x3e], [0xb2], [0x2e], [0xbe]),
        ("__ZN29IOAccelDisplayPipeUserClient229transactionSetPlaneGammaTableEP32IOAccelDisplayPipeGammaTableArgs", [0x3e], [0xa5], [0x2e], [0xb1]),
        ("__ZN29IOAccelDisplayPipeUserClient231transactionSetPipePreGammaTableEP32IOAccelDisplayPipeGammaTableArgs", [0x3e], [0xa5], [0x2e], [0xb1]),
        ("__ZN29IOAccelDisplayPipeUserClient232transactionSetPipePostGammaTableEP32IOAccelDisplayPipeGammaTableArgs", [0x3e], [0xa5], [0x2e], [0xb1]),
        ("__ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs", [0x41, 0x16e], [0x128, 0x1f4], [0x31, 0x15e], [0x134, 0x200]),
        ("__ZN29IOAccelDisplayPipeUserClient215transactionWaitEP37IOAccelDisplayPipeTransactionWaitArgs", [0x4b, 0x11a], [0xce, 0x18b], [0x3b, 0x10a], [0xda, 0x197]),
        ("__ZN29IOAccelDisplayPipeUserClient240transactionSetPipePreCSCLinearizationVIDEP41IOAccelDisplayPipePreCSCLinearizationArgs", [0x3e], [0xa5], [0x2e], [0xb1]),
        ("__ZN29IOAccelDisplayPipeUserClient233transactionSetPipePostCSCGammaVIDEP39IOAccelDisplayPipePostCSCGammaTableArgs", [0x3e], [0xa5], [0x2e], [0xb1]),
        ("__ZN29IOAccelDisplayPipeUserClient211copySurfaceEjj", [0x46], [0xa2], [0x36], [0xae]),
    )
    for method, locks, unlocks, mutexes, unmutexes in display_lock_inventory:
        body = BASE_CLIENT_BODIES.get(method, EVENT_OWNER_BODIES.get(method))
        assert body is not None
        start = address_of(method)
        assert direct_branch_offsets(start, body[0], 0x14ba6da2) == locks and \
            direct_branch_offsets(start, body[0], 0x14ba6db4) == unlocks and \
            direct_branch_offsets(start, body[0], 0x10012) == mutexes and \
            direct_branch_offsets(start, body[0], 0x10018) == unmutexes, \
            f"changed DisplayPipeUserClient lock scope: {method}"
    display_triage = "__ZN29IOAccelDisplayPipeUserClient26triageEPPcPy"
    assert all(direct_branch_offsets(
        address_of(display_triage), BASE_CLIENT_BODIES[display_triage][0], target) == []
        for target in (0x14ba6da2, 0x14ba6db4, 0x10012, 0x10018)), \
        "DisplayPipeUserClient triage gained a lock transition"

    display_start = address_of("__ZN29IOAccelDisplayPipeUserClient25startEP9IOService")
    assert read(display_start + 0x1f, 7) == bytes.fromhex("48 89 83 d8 00 00 00") and \
        read(display_start + 0x2b, 14) == bytes.fromhex(
            "48 8b 80 78 03 00 00 48 89 83 e0 00 00 00") and \
        read(display_start + 0x46, 6) == bytes.fromhex("ff 90 d0 05 00 00"), \
        "changed DisplayPipeUserClient accelerator/display-machine ownership"
    set_pipe = address_of("__ZN29IOAccelDisplayPipeUserClient212setPipeIndexEjPy")
    get_pipe_no_lock = address_of("__ZN29IOAccelDisplayPipeUserClient220getDisplayPipeNoLockEv")
    framebuffer_count = address_of("__ZNK21IOAccelDisplayMachine19getFramebufferCountEv")
    get_display_pipe = address_of("__ZNK21IOAccelDisplayMachine14getDisplayPipeEj")
    assert direct_branch_offsets(set_pipe, 0x168, framebuffer_count) == [0x68] and \
        direct_branch_offsets(set_pipe, 0x168, get_display_pipe) == [0xad] and \
        direct_branch_offsets(get_pipe_no_lock, 0x3a, framebuffer_count) == [0x17] and \
        direct_branch_offsets(get_pipe_no_lock, 0x3a, get_display_pipe) == [0x2d], \
        "changed DisplayPipeUserClient pipe selection edges"
    display_transaction_end = address_of(
        "__ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs")
    display_copy = address_of("__ZN29IOAccelDisplayPipeUserClient211copySurfaceEjj")
    pipe_copy = address_of("__ZN18IOAccelDisplayPipe11copySurfaceEjj")
    assert direct_branch_offsets(
        display_transaction_end, 0x232,
        address_of("__ZN18IOAccelDisplayPipe15transaction_endEP29IOAccelDisplayPipeUserClient2P33IOAccelDisplayPipeTransactionArgs")) == [0x228] and \
        direct_branch_offsets(display_copy, 0xc6, pipe_copy) == [0x73] and \
        read(pipe_copy + 0x174, 6) == bytes.fromhex("ff 90 a8 09 00 00"), \
        "changed DisplayPipe transaction/copy producer roots"

    # A rejected Intel physical framebuffer does not prove this family
    # unreachable.  The inherited display-machine start enumerates registry
    # IOFramebuffer instances and dispatches found_framebuffer; the legacy
    # override delegates to the base creator, which invokes the concrete
    # accelerator newDisplayPipe factory.
    for table, slot, method in (
            ("__ZTV21IOAccelDisplayMachine", 0x850, "__ZN21IOAccelDisplayMachine4initEP22IOGraphicsAccelerator2"),
            ("__ZTV21IOAccelDisplayMachine", 0x858, "__ZN21IOAccelDisplayMachine5startEP11IOPCIDevice"),
            ("__ZTV21IOAccelDisplayMachine", 0x8d8, "__ZN21IOAccelDisplayMachine17found_framebufferEP13IOFramebuffer"),
            ("__ZTV27IOAccelLegacyDisplayMachine", 0x850, "__ZN21IOAccelDisplayMachine4initEP22IOGraphicsAccelerator2"),
            ("__ZTV27IOAccelLegacyDisplayMachine", 0x858, "__ZN27IOAccelLegacyDisplayMachine5startEP11IOPCIDevice"),
            ("__ZTV27IOAccelLegacyDisplayMachine", 0x8d8, "__ZN27IOAccelLegacyDisplayMachine17found_framebufferEP13IOFramebuffer"),
            ("__ZTV22IOGraphicsAccelerator2", 0x908, "__ZN22IOGraphicsAccelerator217createDisplayPipeEP13IOFramebufferj")):
        raw = struct.unpack("<Q", read(address_of(table) + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and \
            raw & 0x3fffffff == address_of(method), \
            f"changed display creation virtual: {table} {slot:#x}"
    display_machine_start = address_of("__ZN21IOAccelDisplayMachine5startEP11IOPCIDevice")
    assert read(display_machine_start + 0xcc, 6) == bytes.fromhex("ff 90 d8 08 00 00") and \
        read(display_machine_start + 0x15d, 6) == bytes.fromhex("ff 91 d8 08 00 00") and \
        read(display_machine_start + 0x19e, 6) == bytes.fromhex("ff 91 d8 08 00 00"), \
        "changed IOFramebuffer enumeration-to-found dispatches"
    legacy_machine_start = address_of("__ZN27IOAccelLegacyDisplayMachine5startEP11IOPCIDevice")
    legacy_found = address_of("__ZN27IOAccelLegacyDisplayMachine17found_framebufferEP13IOFramebuffer")
    base_found = address_of("__ZN21IOAccelDisplayMachine17found_framebufferEP13IOFramebuffer")
    create_pipe = address_of("__ZN22IOGraphicsAccelerator217createDisplayPipeEP13IOFramebufferj")
    assert read(legacy_machine_start + 0xc, 6) == bytes.fromhex("ff a0 68 08 00 00") and \
        read(legacy_found + 0x15, 6) == bytes.fromhex("ff 90 e8 08 00 00") and \
        read(base_found + 0x55, 6) == bytes.fromhex("ff 90 08 09 00 00") and \
        read(create_pipe + 0x17, 6) == bytes.fromhex("ff 90 48 0a 00 00") and \
        read(create_pipe + 0x3c, 6) == bytes.fromhex("ff 90 50 08 00 00"), \
        "changed legacy/base display-pipe creation chain"
    assert read(0x14ba0153, 0x16) == bytes.fromhex(
        "49 8b 06 4c 89 f7 ff 90 00 0a 00 00 49 89 86 78 03 00 00 48 85 c0") and \
        read(0x14ba0191, 0x16) == bytes.fromhex(
            "49 8b 86 78 03 00 00 48 8b 08 48 89 c7 4c 89 f6 ff 91 50 08 00 00"), \
        "changed accelerator display-machine factory/store/init window"
    print("PASS DisplayPipe selectors, locks, producer roots and conservative creation reachability")

    surface_external = address_of(
        "__ZN14IOAccelSurface14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPv")
    assert read(surface_external + 0x115, 6) == bytes.fromhex("ff 90 08 0a 00 00") and \
        read(surface_external + 0x184, 7) == bytes.fromhex("41 ff 92 18 0a 00 00"), \
        "changed special surface set-id/set-shape virtual dispatch"
    assert direct_branch_offsets(
        surface_external, 0x34a,
        address_of("__ZN20IOAccelLegacySurface13surface_flushEjj")) == [0x155], \
        "changed special surface selector-10 flush edge"
    for offset, dispatch in (
            (0xa6, "__ZZN14IOAccelSurface14externalMethodEjP25IOExternalMethodArgumentsP24IOExternalMethodDispatchP8OSObjectPvE19newResourceDispatch"),
            (0xbf, "__ZN14IOAccelSurface20sSignalEventDispatchE")):
        instruction = read(surface_external + offset, 7)
        assert instruction[:3] == bytes.fromhex("48 8d 0d") and \
            surface_external + offset + 7 + struct.unpack_from("<i", instruction, 3)[0] == address_of(dispatch), \
            f"changed special surface dispatch record: {dispatch}"

    # These are the outer accelerator-mutex/busy scopes around inherited
    # surface producer roots.  Inner helpers intentionally contain no second
    # lock acquisition.
    for method, locks, unlocks, mutexes, unmutexes in (
            ("__ZN14IOAccelSurface12surface_readEP22IOAccelSurfaceReadDatay", [0x80], [0x13c], [0x70], [0x148]),
            ("__ZN14IOAccelSurface9set_scaleEjP21IOAccelSurfaceScalingy", [0x56], [0xab], [0x46], [0xb7]),
            ("__ZN14IOAccelSurface15surface_controlEjjPj", [0x48], [0x9a], [0x38], [0xa6]),
            ("__ZN14IOAccelSurface22surface_unlock_optionsE9eLockTypej", [0x3e], [0x115], [0x2e], [0x121]),
            ("__ZN14IOAccelSurface11set_id_modeEjj", [0x81], [0xfb], [0x71], [0x107]),
            ("__ZN14IOAccelSurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony", [0x169], [0x24f, 0x2ba], [0x159], [0x25b, 0x2c6]),
            ("__ZN20IOAccelLegacySurface20surface_lock_optionsE9eLockTypejP25IOAccelSurfaceInformationy", [0x77], [0x10a, 0x2ef, 0x457], [0x67], [0x116, 0x2fb, 0x463]),
            ("__ZN20IOAccelLegacySurface13surface_flushEjj", [0x47, 0x453], [0xd4, 0x3d0, 0x48b], [0x37, 0x443], [0xe0, 0x3dd, 0x497]),
            ("__ZN20IOAccelLegacySurface11set_id_modeEjj", [0x84], [0xfe], [0x74], [0x10a]),
            ("__ZN20IOAccelLegacySurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony", [0x1bf], [0x2cb], [0x1af], [0x2d7]),
            ("__ZN20IOAccelLegacySurface12copy_forwardEjP19IOAccelDeviceRegiony", [0xaf], [0x280], [0x9f], [0x28c]),
            ("__ZN20IOAccelLegacySurface10did_updateEjP19IOAccelDeviceRegiony", [0xad], [0x12e], [0x9d], [0x13a]),
            ("__ZN24IOAccelSharedUserClient222process_dirty_commandsEv", [0x3a], [0x7e], [0x2a], [0x8a])):
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x14ba6da2) == locks, \
            f"changed surface/base-client busy-lock inventory: {method}"
        assert direct_branch_offsets(start, length, 0x14ba6db4) == unlocks, \
            f"changed surface/base-client busy-unlock inventory: {method}"
        assert direct_branch_offsets(start, length, 0x10012) == mutexes, \
            f"changed surface/base-client mutex inventory: {method}"
        assert direct_branch_offsets(start, length, 0x10018) == unmutexes, \
            f"changed surface/base-client mutex-unlock inventory: {method}"

    for method in (
            "__ZN14IOAccelSurface11set_scalingEjP21IOAccelSurfaceScaling",
            "__ZN14IOAccelSurface25surface_control_with_lockEjjPj",
            "__ZN14IOAccelSurface18update_displayableEv",
            "__ZN14IOAccelSurface12update_shapeEv",
            "__ZN14IOAccelSurface12flip_buffersEv",
            "__ZN20IOAccelLegacySurface25present_surface_with_swapEjj",
            "__ZN20IOAccelLegacySurface19submit_scanout_swapEjj",
            "__ZN20IOAccelLegacySurface11submit_swapEjj",
            "__ZN20IOAccelLegacySurface11set_scalingEjP21IOAccelSurfaceScaling",
            "__ZN20IOAccelLegacySurface25surface_control_with_lockEjjPj",
            "__ZN20IOAccelLegacySurface12update_shapeEv",
            "__ZN20IOAccelLegacySurface22submitFullScreenUpdateEj",
            "__ZN14IOAccelShared228processResourceDirtyCommandsEv"):
        start = address_of(method)
        length = BASE_CLIENT_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x14ba6da2) == [] and \
            direct_branch_offsets(start, length, 0x14ba6db4) == [] and \
            direct_branch_offsets(start, length, 0x10012) == [] and \
            direct_branch_offsets(start, length, 0x10018) == [], \
            f"changed selected inner surface/helper lock state: {method}"

    surface_read = address_of("__ZN14IOAccelSurface12surface_readEP22IOAccelSurfaceReadDatay")
    update_displayable = address_of("__ZN14IOAccelSurface18update_displayableEv")
    update_shape = address_of("__ZN14IOAccelSurface12update_shapeEv")
    assert read(surface_read + 0x31e, 6) == bytes.fromhex("ff 90 60 09 00 00"), \
        "changed surface-read copyFromBuffer producer dispatch"
    assert read(update_displayable + 0x145, 6) == bytes.fromhex("ff 90 70 09 00 00") and \
        read(update_shape + 0x2d1, 6) == bytes.fromhex("ff 90 70 09 00 00"), \
        "changed surface swap-copy producer dispatch"
    assert direct_branch_offsets(
        address_of("__ZN14IOAccelSurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony"),
        0x8a0, update_displayable) == [0x6d0], \
        "changed base shape-to-displayable producer edge"
    assert direct_branch_offsets(
        address_of("__ZN20IOAccelLegacySurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony"),
        0xb8c, update_displayable) == [0x9b2], \
        "changed legacy shape-to-displayable producer edge"

    legacy_present = address_of("__ZN20IOAccelLegacySurface25present_surface_with_swapEjj")
    assert direct_branch_offsets(
        legacy_present, 0x128,
        address_of("__ZN20IOAccelLegacySurface19submit_scanout_swapEjj")) == [0xc1] and \
        direct_branch_offsets(
            legacy_present, 0x128,
            address_of("__ZN20IOAccelLegacySurface11submit_swapEjj")) == [0xd1], \
        "changed legacy surface swap selection edges"
    legacy_set_shape = address_of(
        "__ZN20IOAccelLegacySurface9set_shapeE24eIOAccelSurfaceShapeBitsjP19IOAccelDeviceRegiony")
    assert direct_branch_offsets(
        legacy_set_shape, 0x4e,
        address_of("__ZN20IOAccelLegacySurface12copy_forwardEjP19IOAccelDeviceRegiony")) == [0x3c] and \
        direct_branch_offsets(
            legacy_set_shape, 0x4e,
            address_of("__ZN20IOAccelLegacySurface10did_updateEjP19IOAccelDeviceRegiony")) == [0x49], \
        "changed legacy surface shape producer selection"
    assert read(address_of("__ZN20IOAccelLegacySurface19submit_scanout_swapEjj") + 0x9d, 7) == \
        bytes.fromhex("41 ff 92 38 0a 00 00") and \
        read(address_of("__ZN20IOAccelLegacySurface11submit_swapEjj") + 0x2e1, 7) == \
        bytes.fromhex("41 ff 92 38 0a 00 00") and \
        read(address_of("__ZN20IOAccelLegacySurface12copy_forwardEjP19IOAccelDeviceRegiony") + 0x178, 7) == \
        bytes.fromhex("41 ff 92 50 0a 00 00") and \
        read(address_of("__ZN20IOAccelLegacySurface10did_updateEjP19IOAccelDeviceRegiony") + 0xfb, 6) == \
        bytes.fromhex("ff 90 58 0a 00 00") and \
        read(address_of("__ZN20IOAccelLegacySurface22submitFullScreenUpdateEj") + 0x4e, 6) == \
        bytes.fromhex("ff 90 58 0a 00 00"), \
        "changed legacy surface swap/copy/update producer virtuals"

    legacy_surface_vtable = address_of("__ZTV20IOAccelLegacySurface")
    for slot, method in (
            (0x9d8, "__ZN20IOAccelLegacySurface12update_shapeEv"),
            (0x9f0, "__ZN20IOAccelLegacySurface11set_scalingEjP21IOAccelSurfaceScaling"),
            (0xa08, "__ZN20IOAccelLegacySurface11set_id_modeEjj"),
            (0xa10, "__ZN20IOAccelLegacySurface9set_shapeE24eIOAccelSurfaceShapeBitsjP19IOAccelDeviceRegiony"),
            (0xa18, "__ZN20IOAccelLegacySurface28set_shape_backing_length_extE24eIOAccelSurfaceShapeBitsjyjyP19IOAccelDeviceRegiony"),
            (0xa20, "__ZN20IOAccelLegacySurface20surface_lock_optionsE9eLockTypejP25IOAccelSurfaceInformationy"),
            (0xa28, "__ZN14IOAccelSurface22surface_unlock_optionsE9eLockTypej"),
            (0xa30, "__ZN20IOAccelLegacySurface25surface_control_with_lockEjjPj"),
            (0xa40, "__ZN20IOAccelLegacySurface13didSubmitSwapEjj"),
            (0xa48, "__ZN20IOAccelLegacySurface17isBackBufferReadyEj"),
            (0xa50, "__ZN20IOAccelLegacySurface17submitCopyForwardEP12IOAccelEventjP16IOAccelResource2S3_PK13IOAccelBoundsj"),
            (0xa58, "__ZN20IOAccelLegacySurface12submitUpdateEjP13IOAccelBoundsj"),
            (0xa60, "__ZN20IOAccelLegacySurface15pickPresentTypeEj")):
        raw = struct.unpack("<Q", read(legacy_surface_vtable + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and \
            raw & 0x3fffffff == address_of(method), \
            f"changed legacy surface virtual target at {slot:#x}"
    for slot in (0x960, 0x968, 0x970, 0x990, 0x998, 0xa38):
        raw = struct.unpack("<Q", read(legacy_surface_vtable + 16 + slot, 8))[0]
        assert raw == 0x40000000ab59c0, \
            f"changed legacy surface abstract slot at {slot:#x}"
    assert struct.unpack("<Q", read(legacy_surface_vtable + 16 + 0xa68, 8))[0] == \
        0x100000000ab59c0, "changed authenticated legacy surface abstract slot at 0xa68"

    shared_dirty = address_of("__ZN14IOAccelShared228processResourceDirtyCommandsEv")
    for offset, slot in ((0xdb, 0x218), (0xfc, 0x230), (0x113, 0x210), (0x132, 0x238)):
        assert read(shared_dirty + offset, 6) == bytes.fromhex("ff 90") + struct.pack("<I", slot), \
            "changed shared dirty-ring resource-state dispatch"
    print("PASS complete Surface/Device/Shared selectors and inherited surface producer lock graph")

    queue_vtable = address_of("__ZTV19IOAccelCommandQueue")
    queue_submit_dispatch = struct.unpack(
        "<Q4I", read(address_of("__ZN19IOAccelCommandQueue20sCommandQueueMethodsE") + 24, 24))
    assert queue_submit_dispatch[0] >> 63 == 0 and \
        queue_submit_dispatch[0] & 0x3fffffff == address_of(
            "__ZN19IOAccelCommandQueue24s_submit_command_buffersEPS_PvP25IOExternalMethodArguments"), \
        "changed command-queue selector-1 submit target"
    assert queue_submit_dispatch[1:] == (0, 0xffffffff, 0, 0), \
        "changed command-queue selector-1 argument contract"
    for slot, method in (
            (0x9e8, "__ZN19IOAccelCommandQueue16commandQueueStopEv"),
            (0xa28, "__ZN19IOAccelCommandQueue11setPriorityE28eIOAccelCommandQueuePriority"),
            (0xa78, "__ZN19IOAccelCommandQueue22canSubmitCommandBufferEv"),
            (0xa80, "__ZN19IOAccelCommandQueue24pauseSubmitCommandBufferEv")):
        raw = struct.unpack("<Q", read(queue_vtable + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1, "changed command-queue virtual encoding"
        assert raw & 0x3fffffff == address_of(method), "changed command-queue virtual target"
    queue_submit = address_of("__ZN19IOAccelCommandQueue22submit_command_buffersEPK29IOAccelCommandQueueSubmitArgs")
    assert direct_branch_offsets(
        address_of("__ZN19IOAccelCommandQueue24s_submit_command_buffersEPS_PvP25IOExternalMethodArguments"),
        0x130, queue_submit) == [0xf7], "changed external-to-member command-queue submit edge"
    assert direct_branch_offsets(queue_submit, 0x394,
        address_of("__ZN19IOAccelCommandQueue21submit_command_bufferEjjyy")) == [0x30f], \
        "changed per-buffer command-queue submit edge"
    assert direct_branch_offsets(queue_submit, 0x394,
        address_of("__ZN22IOGraphicsAccelerator222acceleratorWaitEnabledEv")) == [0xa8, 0x23c], \
        "changed command-queue enabled-wait inventory"
    assert direct_branch_offsets(queue_submit, 0x394, 0x14ba6da2) == [0x44, 0x21f], \
        "changed command-queue busy-lock inventory"
    assert direct_branch_offsets(queue_submit, 0x394, 0x14ba6db4) == [0x1d5, 0x370], \
        "changed command-queue busy-unlock inventory"
    assert read(queue_submit + 0x168, 14) == bytes.fromhex(
        "48 8b 03 48 89 df ff 90 78 0a 00 00 84 c0"), \
        "changed command-queue can-submit dispatch"
    assert read(queue_submit + 0x1ba, 0x7b) == bytes.fromhex(
        "4c 8b b3 c0 05 00 00 49 8b 06 4c 89 f7 4c 89 fe 31 d2 ff 90 58 08 00 00 4c 89 f7 e8 2e cd ff ff 49 8b be 88 00 00 00 e8 86 5f 46 eb 48 8b 03 48 89 df ff 90 80 0a 00 00 4c 8b b3 c0 05 00 00 4d 8d a6 90 00 00 00 4c 89 e7 e8 7e 60 46 eb 49 8b be 88 00 00 00 e8 52 5f 46 eb 4c 89 e7 e8 64 60 46 eb 4c 89 f7 e8 d2 cc ff ff 49 8b 06 4c 89 f7 4c 89 fe 31 d2 ff 90 50 08 00 00"), \
        "changed command-queue pause unlock/relock window"
    assert read(queue_submit + 0x340, 13) == bytes.fromhex(
        "66 c7 83 15 06 00 00 00 00 48 89 df e8"), \
        "changed command-queue held/admission flag cleanup"
    queue_stop = address_of("__ZN19IOAccelCommandQueue4stopEP9IOService")
    assert direct_branch_offsets(queue_stop, 0x96,
        address_of("__ZN19IOAccelCommandQueue10stopLockedEv")) == [0x5a], \
        "changed command-queue stopLocked edge"
    assert direct_branch_offsets(queue_stop, 0x96, 0x14ba6da2) == [0x3a] and \
        direct_branch_offsets(queue_stop, 0x96, 0x14ba6db4) == [0x7a], \
        "changed command-queue stop busy-lock scope"
    print("PASS command-queue external submit, pause/relock and stop serialization boundary")
    context_vtable = address_of("__ZTV15IOAccelContext2")
    context_submit_entry = struct.unpack(
        "<6Q", read(address_of("__ZN15IOAccelContext215sContextMethodsE") + 2 * 48, 48))
    assert context_submit_entry[0] == 0 and context_submit_entry[1] >> 63 == 0 and \
        context_submit_entry[1] & 0x3fffffff == address_of(
            "__ZN15IOAccelContext219submit_data_buffersEP33IOAccelContextSubmitDataBuffersInP34IOAccelContextSubmitDataBuffersOutyPy"), \
        "changed legacy-context selector-2 submit target"
    assert context_submit_entry[2:] == (0, 3, 0x88, 0xffffffff), \
        "changed legacy-context selector-2 argument contract"
    for slot, method in (
            (0x9e8, "__ZN15IOAccelContext211contextStopEv"),
            (0xa28, "__ZN15IOAccelContext218processDataBuffersEj"),
            (0xa68, GET_DATA_BUFFER),
            (0xab8, "__ZN15IOAccelContext222canSubmitCommandBufferEv"),
            (0xac0, "__ZN15IOAccelContext224pauseSubmitCommandBufferEv")):
        raw = struct.unpack("<Q", read(context_vtable + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1, "changed context virtual encoding"
        assert raw & 0x3fffffff == address_of(method), "changed context virtual target"
    context_submit = address_of(
        "__ZN15IOAccelContext219submit_data_buffersEP33IOAccelContextSubmitDataBuffersInP34IOAccelContextSubmitDataBuffersOutyPy")
    assert direct_branch_offsets(context_submit, 0x972, 0x14ba6da2) == [0x342, 0x492], \
        "changed legacy-context busy-lock inventory"
    assert direct_branch_offsets(context_submit, 0x972, 0x14ba6db4) == [0x448, 0x952], \
        "changed legacy-context busy-unlock inventory"
    for offset, expected in (
            (0x3df, "48 8b 03 48 89 df ff 90 b8 0a 00 00 84 c0"),
            (0x459, "48 8b 03 48 89 df ff 90 c0 0a 00 00"),
            (0x4a9, "48 8b 03 48 89 df ff 90 b8 0a 00 00 84 c0"),
            (0x512, "48 8b 03 48 89 df 8b 75 c8 ff 90 28 0a 00 00")):
        encoded = bytes.fromhex(expected)
        assert read(context_submit + offset, len(encoded)) == encoded, \
            "changed legacy-context submit/pause/process dispatch"
    context_stop = address_of("__ZN15IOAccelContext24stopEP9IOService")
    assert direct_branch_offsets(context_stop, 0xd4, 0x14ba6da2) == [0x3a] and \
        direct_branch_offsets(context_stop, 0xd4, 0x14ba6db4) == [0xb8], \
        "changed legacy-context stop busy-lock scope"
    assert read(context_stop + 0x57, 12) == bytes.fromhex(
        "48 8b 03 48 89 df ff 90 e8 09 00 00"), \
        "changed legacy-context stop dispatch"
    print("PASS legacy-context external submit, pause/relock and stop serialization boundary")
    two_d_vtable = address_of("__ZTV17IOAccel2DContext2")
    for slot, method in (
            (0x9e0, "__ZN17IOAccel2DContext212contextStartEv"),
            (0x9e8, "__ZN17IOAccel2DContext211contextStopEv")):
        raw = struct.unpack("<Q", read(two_d_vtable + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1, \
            "changed 2D-context lifecycle virtual encoding"
        assert raw & 0x3fffffff == address_of(method), \
            "changed 2D-context lifecycle virtual target"
    two_d_methods = address_of("__ZN17IOAccel2DContext217s2DContextMethodsE")
    assert hashlib.sha256(read(two_d_methods, 0x90)).hexdigest() == \
        "d976b38686b08380b806836c3ce684ea690fa33454ebcaeda287018547787e16", \
        "changed complete 2D-context external method table"
    for selector, method, arguments in (
            (0x100, "__ZN17IOAccel2DContext211set_surfaceEj23eIOAccelContextModeBits", (0, 0, 2, 0)),
            (0x101, "__ZN17IOAccel2DContext26finishEj", (0, 0, 1, 0)),
            (0x102, "__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy", (0, 4, 0, 0xffffffff))):
        entry = struct.unpack("<6Q", read(two_d_methods + (selector - 0x100) * 48, 48))
        raw = entry[1]
        assert entry[0] == 0 and raw >> 63 == 0 and (raw >> 30) & 3 == 1 and \
            raw & 0x3fffffff == address_of(method), \
            f"changed 2D-context selector {selector:#x} target"
        assert entry[2:] == arguments, f"changed 2D-context selector {selector:#x} arguments"
    for owner, length, locks, unlocks in (
            ("__ZN17IOAccel2DContext211set_surfaceEj23eIOAccelContextModeBits", 0x338, [0x7f], [0x315]),
            ("__ZN17IOAccel2DContext26finishEj", 0x158, [0x5a], [0xb0]),
            ("__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy", 0x7ae, [0x7c], [0x190, 0x1ff, 0x771])):
        start = address_of(owner)
        assert direct_branch_offsets(start, length, 0x14ba6da2) == locks, \
            f"changed 2D-context busy-lock inventory: {owner}"
        assert direct_branch_offsets(start, length, 0x14ba6db4) == unlocks, \
            f"changed 2D-context busy-unlock inventory: {owner}"
    two_d_blit = address_of("__ZN17IOAccel2DContext24blitEP20IOAccel2DBlitCommandy")
    assert read(two_d_blit + 0x3eb, 6) == bytes.fromhex("ff 90 48 0b 00 00") and \
        read(two_d_blit + 0x5bc, 6) == bytes.fromhex("ff 90 40 0b 00 00"), \
        "changed 2D-context fill/copy virtual dispatch"
    print("PASS 2D-context selectors, complete external bodies, lock scopes and producer dispatch")
    assert read(0x14b6abd9, 10) == bytes.fromhex("48 8d 05 30 79 06 00 48 89 03"), "changed pool allocator concrete vtable install"
    pool_init_pointer = struct.unpack("<Q", read(0x14bd2628, 8))[0]
    assert pool_init_pointer >> 63 == 0 and (pool_init_pointer >> 30) & 3 == 1, "changed pool init cache-level encoding"
    assert pool_init_pointer & 0x3fffffff == address_of("__ZN25IOAccelCommandBufferPool24initEP22IOGraphicsAccelerator2P15IOAccelChannel2P11IOAccelTaskiijjjj"), "changed concrete pool init virtual target"
    raw_free = struct.unpack("<Q", read(address_of(EVENT_VTABLE) + 0xa0, 8))[0]
    assert (raw_free >> 30) & 3 == 1 and raw_free >> 63 == 0, "unexpected event free cache level/auth"
    assert raw_free & 0x3fffffff == address_of("__ZN24IOAccelEventMachineFast24freeEv"), "changed inherited Fast2 free target"
    stop_owner = read(address_of("____ZN20IOAccelEventMachine24stopEv_block_invoke"), 0x8b)
    assert stop_owner.count(bytes.fromhex("ff 90 48 01 00 00")) == 3, "changed base event stop source-removal inventory"
    print("PASS inherited event owner free/stop and per-event stamp-disable bodies (outer drain not proven)")
    lazy_setup = address_of("__ZN18IOAccelDisplayPipe14setup_workloopEv")
    commit_call = read(0x14bbca59, 5)
    assert commit_call[0] == 0xe8 and 0x14bbca5e + struct.unpack_from("<i", commit_call, 1)[0] == address_of("__ZN16IOAccelMemoryMap10commit_pteEv"), "changed mapping prepare PTE commit helper edge"
    assert read(0x14bb77fc, 6) == bytes.fromhex("ff 90 70 01 00 00"), "changed mapping GPU-page-table commit dispatch"
    assert read(0x14bb8fe8, 18) == bytes.fromhex("f6 43 0c 02 74 0c 48 8b 03 48 89 df ff 90 b8 01 00 00"), "changed sys-memory free wired-only unwire branch"
    for call, method in ((0x14bb996b, "__ZN22IOGraphicsAccelerator212sysmem_wiredEP16IOAccelSysMemory"),
                         (0x14bba1f6, "__ZN22IOGraphicsAccelerator212sysmem_wiredEP16IOAccelSysMemory"),
                         (0x14ba5b0b, "__ZN24IOAccelResidentMemorySet9addMemoryEP13IOAccelMemory"),
                         (0x14ba5b74, "__ZN24IOAccelResidentMemorySet12removeMemoryEP13IOAccelMemory")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed wired-resident collection publication/removal edge"
    for table, slot, method in (("__ZTV22IOGraphicsAccelerator2", 0x968, "__ZN22IOGraphicsAccelerator223freeWaitToPrepareSysMapEP16IOAccelMemoryMapb"),
                                ("__ZTV22IOGraphicsAccelerator2", 0x940, "__ZN22IOGraphicsAccelerator223freeWaitToPrepareVidMapEP16IOAccelMemoryMapbb"),
                                ("__ZTV16IOAccelMemoryMap", 0x168, "__ZNK16IOAccelMemoryMap9getLengthEv")):
        raw = struct.unpack("<Q", read(address_of(table) + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1, "changed mapping recovery base virtual encoding"
        assert raw & 0x3fffffff == address_of(method), "changed mapping recovery base virtual identity"
    for call in (0x14b8c00f, 0x14b8c35b):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of("__ZN22IOGraphicsAccelerator220freeToPrepareMappingEP16IOAccelMemoryMap"), "changed resource prepare recovery helper edge"
    for table, slot, method in (("__ZTV16IOAccelMemoryMap", 0x138, "__ZN16IOAccelMemoryMap7prepareEv"),
                                ("__ZTV16IOAccelSysMemory", 0x148, "__ZN13IOAccelMemory7prepareEv"),
                                ("__ZTV16IOAccelSysMemory", 0x150, "__ZN16IOAccelSysMemory8completeEv"),
                                ("__ZTV13IOAccelMemory", 0x150, "__ZN13IOAccelMemory8completeEv")):
        raw = struct.unpack("<Q", read(address_of(table) + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1, "changed map/parent prepare lifecycle encoding"
        assert raw & 0x3fffffff == address_of(method), "changed map/parent prepare lifecycle target"
    cold_call = read(0x14bb77c2, 5)
    assert cold_call[0] == 0xe8 and 0x14bb77c7 + struct.unpack_from("<i", cold_call, 1)[0] == address_of("__ZN16IOAccelMemoryMap7prepareEv.cold.1"), "changed mapping prepare outlined-helper edge"
    assert read(0x14bbca4c, 10) == bytes.fromhex("ff 90 48 01 00 00 84 c0 74 2d"), "changed failed parent prepare skips outer completion"
    complete_table_lea = read(0x14bb9ea3, 7)
    assert complete_table_lea[:3] == bytes.fromhex("48 8d 05") and 0x14bb9eaa + struct.unpack_from("<i", complete_table_lea, 3)[0] == address_of("__ZTV13IOAccelMemory"), "changed sys-memory explicit base complete table"
    assert read(0x14bb9eaa, 6) == bytes.fromhex("ff 90 60 01 00 00"), "changed sys-memory base complete header dispatch"
    factory_forward = read(0x14bb95b6, 5)
    assert factory_forward[0] == 0xe8 and 0x14bb95bb + struct.unpack_from("<i", factory_forward, 1)[0] == address_of("__ZN16IOAccelSysMemory11withOptionsEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2P16IOAccelResource2jyb"), "changed legacy sys-memory factory delegation"
    assert read(0x14bb9954, 9) == bytes.fromhex("05 02 20 00 00 41 89 45 0c"), "changed prewired pool factory flags"
    assert read(0x14bb995d, 8) == bytes.fromhex("41 c7 45 14 00 00 00 00"), "changed prewired pool initial wire count"
    raw_wire = struct.unpack("<Q", read(address_of("__ZTV16IOAccelSysMemory") + 0x1c0, 8))[0]
    assert raw_wire >> 63 == 0 and (raw_wire >> 30) & 3 == 1, "changed base sys-memory wire encoding"
    assert raw_wire & 0x3fffffff == address_of("__ZN16IOAccelSysMemory4wireEv"), "changed explicit Intel-delegated base wire target"
    for address, encoded in ((0x14bba108, "31 d2 ff 91 28 01 00 00"),
                             (0x14bba16b, "31 f6 31 d2 31 c9 45 31 c0 ff 90 40 01 00 00"),
                             (0x14bba18b, "45 31 f6 31 f6 ff 90 30 01 00 00"),
                             (0x14bba1a0, "31 f6 ff 90 f8 01 00 00"),
                             (0x14bba1eb, "80 4b 0c 02")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed sys-memory wire descriptor/prepare/failure cleanup contract"
    for address, encoded in ((0x14bba27a, "31 f6 31 d2 ff 90 48 01 00 00"),
                             (0x14bba2c4, "31 f6 ff 90 30 01 00 00"),
                             (0x14bba2fd, "e8 0a 60 45 eb"),
                             (0x14ba8335, "48 8b bb 18 0a 00 00"),
                             (0x14ba8347, "3b 83 1c 0d 00 00 73 3f"),
                             (0x14ba8394, "ff 50 28")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed DMA return cleanup/status/lock-capacity contract"
    assert read(0x14ba0f35, 6) == bytes.fromhex("ff 90 b0 0a 00 00"), "changed accelerator DMA pool factory virtual"
    assert read(0x14ba1034, 6) == bytes.fromhex("ff 90 18 01 00 00"), "changed accelerator DMA template clone virtual"
    assert read(0x14ba22d3, 10) == bytes.fromhex("48 8b bb 10 0a 00 00 48 8b 07"), "changed DMA pool template final-release dereference"
    raw_unwire = struct.unpack("<Q", read(address_of("__ZTV16IOAccelSysMemory") + 0x1c8, 8))[0]
    assert raw_unwire >> 63 == 0 and (raw_unwire >> 30) & 3 == 1, "changed base sys-memory unwire encoding"
    assert raw_unwire & 0x3fffffff == address_of("__ZN16IOAccelSysMemory6unwireEv"), "changed base sys-memory unwire target"
    # These are selected complete failure-path windows, NOT a claim that the
    # entire inherited accelerator start body has been reviewed here.
    for start, length, digest in (
            (0x14b9ff23, 0x1a, "9296105c846353d285040948e48173f53bd680989e8559d4e3ca397d96adcffe"),
            (0x14ba07b5, 0x82, "2aa1d164ff8b96d52162ec54a1ae52b15e0e5aef5c4995e5117e9348cad71be2"),
            (0x14b9fbc3, 0x2f, "6f3bd9c892c2647af39bbcc1455d34a152ee4ee4fe06fd58961b860497ebfa02"),
            (0x14ba1e29, 0x12, "633db41e86ae7590862cdf404d6de633c7e63b6e79ba7114f91ba8e8ae259ae2")):
        assert hashlib.sha256(read(start, length)).hexdigest() == digest, "changed DMA-pool failed-start/stop window"
    for call, target in ((0x14b9ff30, address_of("__ZN22IOGraphicsAccelerator220createDMACommandPoolEv")),
                         (0x14b9fbed, 0x14ba1a7c),
                         (0x14ba1e36, address_of("__ZN22IOGraphicsAccelerator221releaseDMACommandPoolEv"))):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == target, "changed inherited failed-start pool cleanup edge"
    for call, method in ((0x14bba340, "__ZN16IOAccelMemoryMap11release_pteEv"),
                         (0x14bba30d, "__ZN22IOGraphicsAccelerator216returnDMACommandEP12IODMACommand"),
                         (0x14bba3bc, "__ZN22IOGraphicsAccelerator214sysmem_unwiredEP16IOAccelSysMemory")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed unwire cleanup edge"
    assert read(0x14bb8fe8, 4) == bytes.fromhex("f6 43 0c 02"), "changed sys-memory free conditional unwire flag"
    assert read(0x14bb74de, 9) == bytes.fromhex("ff 90 78 01 00 00 8b 43 10"), "changed ignored Intel release result before installed-flag reload"
    assert read(0x14bb74e7, 6) == bytes.fromhex("83 e0 fb 89 43 10"), "changed unconditional installed-PTE flag clearing"
    assert read(0x14bba33a, 6) == bytes.fromhex("f6 47 10 04 74 09"), "changed unwire installed-PTE-only release admission"
    assert read(0x14bba369, 6) == bytes.fromhex("ff 90 f8 01 00 00"), "changed descriptor complete after mapping PTE releases"
    assert read(0x14bb8ff4, 6) == bytes.fromhex("ff 90 b8 01 00 00"), "changed sys-memory free unwire dispatch"
    assert read(0x14bb736d, 20) == bytes.fromhex("f6 47 10 01 74 7f 48 8b 03 48 89 df ff 50 18 83 f8 01 75 71"), "changed mapping last-reference release admission"
    assert read(0x14bb740c, 6) == bytes.fromhex("83 c8 08 89 43 10"), "changed deferred mapping flag publication"
    raw_map_release = struct.unpack("<Q", read(address_of("__ZTV16IOAccelMemoryMap") + 0x38, 8))[0]
    assert raw_map_release >> 63 == 0 and (raw_map_release >> 30) & 3 == 1, "changed mapping release vtable encoding"
    assert raw_map_release & 0x3fffffff == address_of("__ZNK16IOAccelMemoryMap7releaseEv"), "changed mapping release vtable target"
    for call, method in ((0x14b9e27a, "__ZN16IOAccelMemoryMap11finishEventEv"),
                         (0x14b9e450, "__ZN16IOAccelMemoryMap9testEventEv"),
                         (0x14b9e7c2, "__ZN16IOAccelMemoryMap11release_pteEv")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed task mapping cleanup edge"
    assert read(0x14b959c0, 2) == bytes.fromhex("74 0f"), "changed event test conditional cleanup"
    assert read(0x14b95465, 9) == bytes.fromhex("83 b9 c8 0d 00 00 00 74 0d"), "changed termination bypass in mapping event test"
    for call, target in ((0x14ba12a0, 0x10012),
                         (0x14ba138b, 0x10012),
                         (0x14ba12da, address_of("__ZN22IOGraphicsAccelerator222free_orphaned_gputasksEv")),
                         (0x14ba1414, address_of("__ZN22IOGraphicsAccelerator222free_orphaned_gputasksEv")),
                         (0x14ba150a, 0x10018),
                         (0x14ba5839, address_of("__ZN11IOAccelTask22free_orphaned_mappingsEv")),
                         (0x14b9e143, address_of("__ZN11IOAccelTask22free_orphaned_mappingsEv")),
                         (0x14b9e1b1, address_of("__ZN16IOAccelMemoryMap11release_pteEv"))):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == target, "changed serialized collector/task cleanup edge"
    assert read(0x14ba1299, 7) == bytes.fromhex("48 8b bb 88 00 00 00"), "changed garbage collector mutex identity"
    assert read(0x14ba1384, 7) == bytes.fromhex("48 8b bf 88 00 00 00"), "changed GART collector mutex identity"
    for method in ("__ZN14IOAccelShared24freeEv",
                   "__ZNK11IOAccelTask7releaseEv",
                   "__ZN11IOAccelTask18freeAllGPUMappingsEv",
                   "__ZNK16IOAccelMemoryMap7releaseEv",
                   "__ZN16IOAccelMemoryMap11release_pteEv"):
        start = address_of(method)
        length = EVENT_OWNER_BODIES[method][0]
        assert direct_branch_offsets(start, length, 0x10012) == [], f"changed selected lock-free cleanup entry: {method}"
        assert direct_branch_offsets(start, length, 0x10018) == [], f"changed selected unlock-free cleanup entry: {method}"
    assert direct_branch_offsets(0x14ba1280, 0xce, 0x10012) == [0x20], "changed garbage collector lock entry"
    assert direct_branch_offsets(0x14ba1280, 0xce, 0x10018) == [0xc8], "changed garbage collector unlock exit"
    assert direct_branch_offsets(0x14ba1376, 0x1d4, 0x10012) == [0x15], "changed GART collector lock entry"
    assert direct_branch_offsets(0x14ba1376, 0x1d4, 0x10018) == [0x194, 0x1ce], "changed GART collector unlock exits"
    for call, method in ((0x14b8e78d, "__ZN25IOAccelOrphanedMemoryPool13sharedReleaseEP14IOAccelShared2"),
                         (0x14b8e79e, "__ZN25IOAccelOrphanedMemoryPool13sharedReleaseEP14IOAccelShared2"),
                         (0x14b8e90d, "__ZN11IOAccelTask23prune_orphaned_mappingsEv")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed Shared teardown cleanup edge"
    assert read(0x14b8e91c, 14) == bytes.fromhex("ff 50 28 48 c7 83 88 00 00 00 00 00 00 00"), "changed Shared task release/identity clear ordering"
    raw_shared_release = struct.unpack("<Q", read(address_of(RESOURCE_VTABLE) + 16 + 0x160, 8))[0]
    edge = read(0x14b9e05f, 5)
    assert edge[0] == 0xe8 and 0x14b9e064 + struct.unpack_from("<i", edge, 1)[0] == address_of("__ZN15IOAccelTaskList10removeTaskEP11IOAccelTask"), "changed base task final list unlink edge"
    for table, slot, method in (("__ZTV24IOAccelSharedUserClient2", 0x990, "__ZN24IOAccelSharedUserClient211sharedStartEv"),
                                ("__ZTV22IOGraphicsAccelerator2", 0x8b0, "__ZN22IOGraphicsAccelerator212createSharedEP4task"),
                                (SHARED_VTABLE, 0x118, "__ZN14IOAccelShared24initEP22IOGraphicsAccelerator2P4task")):
        raw = struct.unpack("<Q", read(address_of(table) + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and raw & 0x3fffffff == address_of(method), "changed Shared construction declared virtual"
    for call, target in ((0x14b90129, 0x10012), (0x14b90192, 0x10018)):
        edge = read(call, 5)
        assert edge[0] == 0xe8 and call + 5 + struct.unpack_from("<i", edge, 1)[0] == target, "changed Shared start outer mutex edge"
    assert read(0x14b90122, 7) == bytes.fromhex("49 8b be 88 00 00 00"), "changed Shared construction mutex identity"
    assert read(0x14b9015c, 6) == bytes.fromhex("ff 90 90 09 00 00"), "changed sharedStart dispatch inside outer mutex"
    base_task_init = struct.unpack("<Q", read(address_of("__ZTV11IOAccelTask") + 0x128, 8))[0]
    assert base_task_init >> 63 == 0 and (base_task_init >> 30) & 3 == 1 and base_task_init & 0x3fffffff == address_of("__ZN11IOAccelTask4initEP22IOGraphicsAccelerator2jPP16IORangeAllocator"), "changed base task init header-relative dispatch"
    edge = read(0x14b9df26, 5)
    assert edge[0] == 0xe8 and 0x14b9df2b + struct.unpack_from("<i", edge, 1)[0] == address_of("__ZN15IOAccelTaskList7addTaskEP11IOAccelTask"), "changed base task raw list publication edge"
    for call, target in ((0x14b914b4, 0x10012), (0x14b9156f, 0x10018),
                         (0x14b914f0, address_of("__ZNK16IOAccelNamespace8lookupIdEjPPv"))):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == target, "changed resource delete mutex/lookup edge"
    assert read(0x14b914ac, 8) == bytes.fromhex("49 8b bc 24 88 00 00 00"), "changed resource delete accelerator mutex field"
    assert read(0x14b91513, 6) == bytes.fromhex("ff 90 60 01 00 00"), "changed resource delete Shared-release dispatch"
    assert raw_shared_release >> 63 == 0 and (raw_shared_release >> 30) & 3 == 1 and raw_shared_release & 0x3fffffff == address_of("__ZN16IOAccelResource213sharedReleaseEP14IOAccelShared2"), "changed declared resource Shared-release target"
    raw_shared_stop = struct.unpack("<Q", read(address_of("__ZTV24IOAccelSharedUserClient2") + 16 + 0x998, 8))[0]
    assert raw_shared_stop >> 63 == 0 and (raw_shared_stop >> 30) & 3 == 1 and raw_shared_stop & 0x3fffffff == address_of("__ZN24IOAccelSharedUserClient210sharedStopEv"), "changed declared Shared user-client stop target"
    for call, target in ((0x14b90323, 0x10012), (0x14b903bd, 0x10018),
                         (0x14b907ca, 0x10012), (0x14b907e7, 0x10018)):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == target, "changed Shared user-client mutex edge"
    assert read(0x14b9031c, 7) == bytes.fromhex("49 8b be 88 00 00 00"), "changed user-client stop accelerator mutex field"
    assert read(0x14b90366, 6) == bytes.fromhex("ff 90 98 09 00 00"), "changed sharedStop invocation inside stop mutex scope"
    for call, target in ((0x14ba63ee, 0x10012), (0x14ba6573, 0x10018),
                         (0x14ba6530, address_of("__ZN11IOAccelTask23prune_orphaned_mappingsEv")),
                         (0x14ba5f78, address_of("__ZN11IOAccelTask22free_orphaned_mappingsEv")),
                         (0x14ba5fca, address_of("__ZN11IOAccelTask22free_orphaned_mappingsEv"))):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == target, "changed sleep/wake cleanup and mutex edge"
    assert direct_branch_offsets(0x14ba63a0, 0x20c, 0x10012) == [0x4e], "changed wake cleanup lock entry"
    assert direct_branch_offsets(0x14ba63a0, 0x20c, 0x10018) == [0x1d3], "changed wake cleanup unlock exit"
    # Pin an UNREPAIRED reference defect, not a successful cleanup invariant:
    # the orphan-list iterator is built at -0x48 but read at exhausted -0x30.
    assert read(0x14ba5f7f, 4) == bytes.fromhex("48 8d 7d b8"), "changed sleep second iterator construction slot"
    assert read(0x14ba5f9a, 4) == bytes.fromhex("48 8d 7d d0"), "changed unrepaired sleep second iterator read slot"
    assert read(0x14ba5fab, 4) == bytes.fromhex("4c 8d 75 d0"), "changed unrepaired sleep iterator loop slot"
    raw_sleep = struct.unpack("<Q", read(address_of("__ZTV22IOGraphicsAccelerator2") + 16 + 0x9d8, 8))[0]
    assert raw_sleep >> 63 == 0 and (raw_sleep >> 30) & 3 == 1 and raw_sleep & 0x3fffffff == address_of("__ZN22IOGraphicsAccelerator215systemWillSleepEv"), "changed inherited sleep target"
    for call, target in ((0x14ba618e, 0x10012), (0x14ba627c, 0x10018),
                         (0x14ba6306, 0x10012), (0x14ba636f, 0x10018)):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == target, "changed system-will-sleep mutex edge"
    assert read(0x14ba61cf, 6) == bytes.fromhex("ff 90 d8 09 00 00"), "changed sleep virtual dispatch inside mutex scope"
    assert direct_branch_offsets(0x14ba6144, 0x25c, 0x10012) == [0x4a, 0x1c2], "changed sleep cleanup lock entries"
    assert direct_branch_offsets(0x14ba6144, 0x25c, 0x10018) == [0x138, 0x22b], "changed sleep cleanup unlock exits"
    for slot, name in ((0x140, "__ZN11IOAccelTask8allocateEPK16IOAccelMemoryMap"),
                       (0x148, "__ZN11IOAccelTask10deallocateEPK16IOAccelMemoryMapy")):
        raw = struct.unpack("<Q", read(address_of("__ZTV11IOAccelTask") + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and raw & 0x3fffffff == address_of(name), "changed declared task allocator dispatch"
    for call, method in ((0x14b6752b, "__ZN11IOAccelTask23prune_orphaned_mappingsEv"),
                         (0x14b67543, "__ZN11IOAccelTask22free_orphaned_mappingsEv"),
                         (0x14b674c8, "__ZN20IOAccelMemoryMapList13removeMappingEP16IOAccelMemoryMap"),
                         (0x14b674da, "__ZN20IOAccelMemoryMapList10addMappingEP16IOAccelMemoryMap"),
                         (0x14bb76a5, "__ZN20IOAccelMemoryMapList13removeMappingEP16IOAccelMemoryMap")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed mapping VA reuse/cleanup edge"
    for slot, name in ((0x150, "__ZN16IOAccelMemoryMap22allocGPUVirtualAddressEv"),
                       (0x158, "__ZN16IOAccelMemoryMap24reserveGPUVirtualAddressEyy"),
                       (0x160, "__ZN16IOAccelMemoryMap21freeGPUVirtualAddressEv")):
        raw = struct.unpack("<Q", read(address_of("__ZTV16IOAccelMemoryMap") + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1 and raw & 0x3fffffff == address_of(name), "changed declared mapping VA lifecycle target"
    for call, method in ((0x14bb73da, "__ZN16IOAccelMemoryMap11release_pteEv"),
                         (0x14bb7421, "__ZN20IOAccelMemoryMapList13removeMappingEP16IOAccelMemoryMap"),
                         (0x14bb7435, "__ZN20IOAccelMemoryMapList10addMappingEP16IOAccelMemoryMap"),
                         (0x14bb743e, "__ZN13IOAccelMemory18check_orphan_stateEv")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed mapping immediate/deferred release edge"
    assert read(0x14b675ce, 3) == bytes.fromhex("ff 4f 10"), "changed parent memory complete count decrement"
    raw_map_free = struct.unpack("<Q", read(address_of("__ZTV16IOAccelMemoryMap") + 0xa0, 8))[0]
    assert raw_map_free >> 63 == 0 and (raw_map_free >> 30) & 3 == 1, "changed memory-map free encoding"
    assert raw_map_free & 0x3fffffff == address_of("__ZN16IOAccelMemoryMap4freeEv"), "changed memory-map base free target"
    remove_call = read(0x14bb751e, 5)
    assert remove_call[0] == 0xe8 and 0x14bb7523 + struct.unpack_from("<i", remove_call, 1)[0] == address_of("__ZN13IOAccelMemory14remove_mappingEP16IOAccelMemoryMap"), "changed memory-map parent inventory removal"
    for call, method in ((0x14ba608d, "__ZN30IOAccelDisplayPipeTransaction26finishEv"),
                         (0x14ba6095, "__ZN30IOAccelDisplayPipeTransaction28completeEv"),
                         (0x14baecc8, "__ZN22IOGraphicsAccelerator226accel_transaction_finishedEP26DisplayTransactionListHead"),
                         (0x14baedb8, "__ZN22IOGraphicsAccelerator226accel_transaction_finishedEP26DisplayTransactionListHead"),
                         (0x14b8c447, "__ZN16IOAccelMemoryMap15remove_resourceEP16IOAccelResource2")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed finished transaction/backing inventory cleanup edge"
    assert read(0x14ba60a0, 3) == bytes.fromhex("ff 50 28"), "changed finished transaction final release"
    for slot, method in ((0x170, "__ZN16IOAccelResource27prepareEv"),
                         (0x178, "__ZN16IOAccelResource28completeEv"),
                         (0x190, "__ZN16IOAccelResource212addToChannelEP15IOAccelChannel2j"),
                         (0x198, "__ZN16IOAccelResource217removeFromChannelEP15IOAccelChannel2")):
        raw = struct.unpack("<Q", read(address_of(RESOURCE_VTABLE) + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1, "changed resource lifecycle virtual encoding"
        assert raw & 0x3fffffff == address_of(method), "changed resource lifecycle virtual target"
    assert read(0x14b8c412, 6) == bytes.fromhex("ff 4f 28 74 01 c3"), "changed resource complete decrement/zero cleanup edge"
    assert read(0x14b8c01c, 3) == bytes.fromhex("ff 43 28"), "changed first resource prepare count increment"
    legacy_submit = struct.unpack("<Q", read(address_of("__ZTV24IOAccelLegacyDisplayPipe") + 0x8c8, 8))[0]
    assert legacy_submit >> 63 == 0 and (legacy_submit >> 30) & 3 == 1, "changed legacy display submit cache level/auth"
    assert legacy_submit & 0x3fffffff == address_of("__ZN18IOAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2"), "changed explicit legacy-table submit target"
    raw_action, scalar_in, struct_in, scalar_out, struct_out = struct.unpack("<Q4I", read(0x14be3950 + 8 * 24, 24))
    assert raw_action >> 63 == 0 and (raw_action >> 30) & 3 == 1, "changed transaction-end descriptor target encoding"
    assert raw_action & 0x3fffffff == address_of("__ZN29IOAccelDisplayPipeUserClient217s_transaction_endEPS_PvP25IOExternalMethodArguments"), "changed selector 8 action"
    assert (scalar_in, struct_in, scalar_out, struct_out) == (0, 280, 0, 0), "changed transaction-end argument counts"
    for call, target in ((0x14bb516d, "__ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs"),
                         (0x14bb6456, "__ZN18IOAccelDisplayPipe15transaction_endEP29IOAccelDisplayPipeUserClient2P33IOAccelDisplayPipeTransactionArgs"),
                         (0x14bb0602, "__ZN30IOAccelDisplayPipeTransaction220set_transaction_argsEP33IOAccelDisplayPipeTransactionArgs"),
                         (0x14bb06a3, "__ZN18IOAccelDisplayPipe17transaction_queueEP30IOAccelDisplayPipeTransaction2")):
        encoded = read(call, 5)
        assert encoded[0] in (0xe8, 0xe9) and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(target), "changed transaction-end dispatch/queue edge"
    for call in (0x14bb08ca, 0x14bb0d1a):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of("__ZN18IOAccelDisplayPipe38set_current_plane_ioSurfaceDeviceCacheEP12IOAccelEventjjP20IOSurfaceDeviceCache"), "changed transaction plane-cache replacement edge"
    for address, encoded in ((0x14bb32a6, "ff 90 78 01 00 00"),
                             (0x14bb335a, "ff 90 78 01 00 00"),
                             (0x14bb3390, "ff 90 70 01 00 00"),
                             (0x14bb3396, "84 c0 74 5f")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed plane-cache prepare/complete lifecycle anchor"
    for call, method in ((0x14b6b4b8, "__ZN25IOAccelCommandBufferPool221setBufferCurrentIndexEs"),
                         (0x14b6b331, "__ZN25IOAccelCommandBufferPool221setBufferCurrentIndexEs"),
                         (0x14b6b2ed, "__ZN25IOAccelCommandBufferPool212submitBufferEv"),
                         (0x14b6b4d3, "__ZN16IOAccelMemoryMap9testEventEv"),
                         (0x14b6b50e, "__ZN16IOAccelMemoryMap11finishEventEv")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed command-pool selection/reuse-event edge"
    for address, encoded in ((0x14b6b204, "66 41 89 9e 42 18 00 00"),
                             (0x14b6b21b, "49 c7 47 08 00 00 00 00"),
                             (0x14b6b280, "49 c7 47 08 00 00 00 00"),
                             (0x14b6b51f, "48 8b 44 c3 40")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed command-pool current-index publication/failure cleanup anchor"
    assert read(0x14bb1134, 6) == bytes.fromhex("ff 90 a0 08 00 00"), "changed transaction argument validation dispatch"
    assert read(0x14bb0607, 15) == bytes.fromhex("85 c0 74 0b 41 89 c4 89 43 58 e9 83 00 00 00"), "changed validation error preservation before queue cleanup"
    assert read(0x14bb061e, 6) == bytes.fromhex("41 89 c4 89 43 58"), "changed transaction preparation result preservation"
    for slot, method in ((0x868, "__ZN18IOAccelDisplayPipe21displayModeWillChangeEv"),
                         (0x8b8, "__ZN18IOAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2"),
                         (0x8d8, "__ZN18IOAccelDisplayPipe16beginTransactionEP12IOAccelEvent"),
                         (0x8e8, "__ZN18IOAccelDisplayPipe21framebufferTerminatedEv")):
        raw = struct.unpack("<Q", read(address_of("__ZTV18IOAccelDisplayPipe") + 16 + slot, 8))[0]
        assert raw >> 63 == 0 and (raw >> 30) & 3 == 1, "changed base display terminal virtual cache level/auth"
        assert raw & 0x3fffffff == address_of(method), "changed base display terminal virtual target"
    for address, encoded in ((0x14bb211b, "41 89 46 58"),
                             (0x14bb242b, "48 c7 83 48 02 00 00 00 00 00 00"),
                             (0x14bb2436, "4c 89 b3 50 02 00 00"),
                             (0x14bb41ce, "f6 87 40 01 00 00 01"),
                             (0x14bb4236, "f6 87 40 01 00 00 01"),
                             (0x14bb3834, "4c 89 bb 58 01 00 00"),
                             (0x14bb2165, "0f ae f8"),
                             (0x14bb007d, "80 8b 40 01 00 00 01"),
                             (0x14bb3e9d, "b9 0b 00 00 00")):
        assert read(address, len(bytes.fromhex(encoded))) == bytes.fromhex(encoded), "changed transaction status/publication/prepare/notification edge"
    for address, encoded in ((0x14bb59e2, "48 89 83 e8 00 00 00"),
                            (0x14bb5a79, "ff 50 20"),
                            (0x14bb5793, "ff 50 28"),
                            (0x14bb5796, "48 c7 83 e8 00 00 00 00 00 00 00"),
                            (0x14baf1b1, "c6 87 80 02 00 00 01"),
                            (0x14baf24d, "48 c7 83 98 00 00 00 00 00 00 00")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed display pipe ownership/terminal ordering anchor"
    for call, target in ((0x14bafbed, "__ZN18IOAccelDisplayPipe26wait_for_queue_slot_nolockEv"),
                         (0x14bb636a, "__ZN18IOAccelDisplayPipe26wait_for_queue_slot_nolockEv"),
                         (0x14bb5758, "__ZN18IOAccelDisplayPipe13remove_notifyEP31IOAccelDisplayPipeNotifyRequest"),
                         (0x14bb5d43, "__ZN18IOAccelDisplayPipe14request_notifyEPyP40IOAccelDisplayPipeRequestNotifyGatedArgs")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(target), "changed display upstream lazy-operation caller edge"
    assert read(0x14bb5d48, 3) == bytes.fromhex("41 89 c6"), "changed requestNotify gate result propagation"
    assert read(0x14bb636f, 7) == bytes.fromhex("4c 8b ab d8 00 00 00"), "changed transactionEnd ignored-wait-result cleanup edge"
    for call, load in ((0x14bb017a, 0x14bb017f), (0x14bb01c6, 0x14bb01cb),
                       (0x14bb2f08, 0x14bb2f0d), (0x14bb2fec, 0x14bb2ff1)):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == lazy_setup, "changed display lazy setup direct edge"
        assert read(load, 3) in (bytes.fromhex("48 8b bb"), bytes.fromhex("49 8b be"), bytes.fromhex("49 8b bf")) and \
            read(load + 3, 4) == bytes.fromhex("b0 00 00 00") and \
            read(load + 7, 3) == bytes.fromhex("48 8b 07"), "changed unchecked post-setup gate dereference"
    raw_pipe_factory = struct.unpack("<Q", read(address_of("__ZTV18IOAccelDisplayPipe") + 16 + 0x8c8, 8))[0]
    assert (raw_pipe_factory >> 30) & 3 == 1 and raw_pipe_factory >> 63 == 0, "unexpected base display pipe factory cache level/auth"
    assert raw_pipe_factory & 0x3fffffff == address_of("__ZN18IOAccelDisplayPipe14createWorkLoopEv"), "changed base display pipe createWorkLoop virtual"
    for address, encoded in ((0x14bb1612, "31 f6"),
                            (0x14bb1619, "48 89 83 b0 00 00 00"),
                            (0x14bb1632, "ff 91 40 01 00 00"),
                            (0x14bb1638, "48 8b bb b0 00 00 00")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed display gate null-action/store/unchecked attachment"
    for lea, target in ((0x14baec6f, "__ZN18IOAccelDisplayPipe28transaction_queue_idle_gatedEv"),
                        (0x14baeca4, "__ZN18IOAccelDisplayPipe31get_finished_transactions_gatedEP26DisplayTransactionListHead"),
                        (0x14baedde, "__ZN18IOAccelDisplayPipe23teardown_workloop_gatedEv"),
                        (0x14baed5f, "__ZN18IOAccelDisplayPipe30release_live_transaction_gatedEv"),
                        (0x14baed94, "__ZN18IOAccelDisplayPipe31get_finished_transactions_gatedEP26DisplayTransactionListHead")):
        encoded = read(lea, 7)
        assert encoded[:3] == bytes.fromhex("48 8d 35") and lea + 7 + struct.unpack_from("<i", encoded, 3)[0] == address_of(target), "changed display cleanup gated action identity"
    teardown_sources = read(address_of("__ZN18IOAccelDisplayPipe23teardown_workloop_gatedEv"), 0x132)
    assert teardown_sources.count(bytes.fromhex("ff 90 48 01 00 00")) == 5 and \
        teardown_sources.count(bytes.fromhex("48 8b 80 48 01 00 00")) == 1, "changed six-source display teardown removal inventory"
    for address, encoded in ((0x14ba1add, "49 8b be 80 03 00 00"),
                            (0x14ba1aec, "ff 90 58 01 00 00"),
                            (0x14ba1af2, "49 8b be 80 03 00 00"),
                            (0x14ba1afc, "ff 90 68 02 00 00")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed inherited accelerator event finish-before-stop order"
    # There are multiple real local definitions, not one ambiguous address to
    # pick arbitrarily. Require the full reviewed copy inventory and bodies.
    for name, copies in LOCK_COPIES.items():
        assert sorted(matches[name]) == sorted(copies), f"changed local mutex copy inventory: {name}"
        length = 0x52 if "acceleratorLockEv" in name else 0x36
        for address, digest in copies.items():
            assert hashlib.sha256(read(address, length)).hexdigest() == digest, f"changed mutex copy {address:#x}"
    print("PASS all four local mutex-lock and five mutex-unlock bodies")
    buffer_start = address_of(GET_DATA_BUFFER)
    buffer_body = read(buffer_start, 0x9e4)
    assert hashlib.sha256(buffer_body).hexdigest() == \
        "7840d4fbada0dbedc42efd2896dcafefcc54b25c03833e00cf57b55e2ab7607b", "changed getDataBuffer"
    # Helper-only call graph scans miss the second, inlined unlock/lock window.
    for offset, target in ((0x244, 0x14b6c7f8), (0x26f, 0x14b6c7a6),
                           (0x552, 0x10018), (0x593, 0x10012)):
        assert buffer_body[offset] == 0xe8, "changed buffer wait lock edge"
        assert buffer_start + offset + 5 + struct.unpack_from("<i", buffer_body, offset + 1)[0] == target, \
            "changed buffer wait lock target"
    for offset in (0x25e, 0x56c):
        assert buffer_body[offset:offset + 6] == bytes.fromhex("ff 90 78 01 00 00"), "changed unlocked event wait"
    for offset in (0x29b, 0x5e9):
        assert buffer_body[offset:offset + 4] == bytes.fromhex("4d 8b 6e 38"), "changed post-wait buffer reload"
    print("PASS getDataBuffer helper and inlined mutex-release wait windows")
    unlocked_start = address_of(EVENT_FINISH_UNLOCKED)
    unlocked_body = read(unlocked_start, 0x194)
    assert hashlib.sha256(unlocked_body).hexdigest() == \
        "fb78fffbb00c991767abc188fa7bbf743432de41b03ddd3c839d008963809dc2", "changed finishEventUnlocked"
    assert unlocked_body[0x6f:0x80] == bytes.fromhex(
        "49 8b 4e 10 83 b9 c8 0d 00 00 00 0f 85 f7 00 00 00"), "changed unlocked finish termination bypass"
    assert unlocked_body[0xdc:0xe2] == bytes.fromhex("ff 90 38 02 00 00"), "changed unlocked stamp wait virtual"
    assert unlocked_body[0xfa] == 0xe8 and unlocked_start + 0xff + struct.unpack_from("<i", unlocked_body, 0xfb)[0] == \
        address_of(EVENT_HARDWARE_ERROR), "changed unlocked timeout error request"
    assert hashlib.sha256(read(address_of(EVENT_HARDWARE_ERROR), 0x112)).hexdigest() == \
        "fa0c96832f317613aa5389d04fdcae250b356be84cac4c009438b77b96afd649", "changed software hardware-error signaling"
    print("PASS inherited unlocked finish retry and software error-request identities")
    for table, slot, name in ((SHARED_VTABLE, 0x128, SHARED_SCRUB),
                              (RESOURCE_VTABLE, 0x228, RESOURCE_SCRUB),
                              (EVENT_VTABLE, 0x270, EVENT_SCRUB)):
        raw = struct.unpack("<Q", read(address_of(table) + 16 + slot, 8))[0]
        assert (raw >> 30) & 3 == 1 and raw >> 63 == 0, "unexpected scrub cache level/auth"
        assert raw & 0x3fffffff == address_of(name), f"changed scrub virtual {slot:#x}"
    scrub = read(address_of(EVENT_SCRUB), 0x7e)
    assert scrub[0x58:0x66] == bytes.fromhex(
        "4c 8b 47 10 41 83 b8 c8 0d 00 00 00 74 0f"), "changed scrub termination bypass"
    assert scrub[0x66:0x6a] == bytes.fromhex("48 89 14 ce"), "changed scrub event clearing"
    print("PASS shared/resource/memory scrub graph and non-hardware termination bypass")
    # XNU EXTERNAL_HEADERS/mach-o/fixup-chains.h kernel-cache rebase:
    # target:30, cacheLevel:2, next:12, isAuth:1. This archived SystemKC
    # level-1 unslid base is zero; never apply this to a live slid pointer.
    for slot, name in ((0x150, "__ZN24IOAccelEventMachineFast211finishStampEi"),
                       (0x190, "__ZN24IOAccelEventMachineFast29testEventEP12IOAccelEvent"),
                       (0x180, "__ZN24IOAccelEventMachineFast217testEventUnlockedEP12IOAccelEvent"),
                       (0x188, EVENT_FINISH), (0x238, EVENT_WAIT),
                       (0x148, EVENT_CLEAN), (0x250, EVENT_TERMINATE), (0x228, EVENT_SIGNAL),
                       (0x1c8, EVENT_MERGE_EXCLUDING), (0x1d0, EVENT_SET_STAMP),
                       (0x1d8, EVENT_INCREMENT), (0x1e0, EVENT_WRITE_STAMP),
                       (0x140, EVENT_INIT), (0x1b0, EVENT_COPY), (0x178, EVENT_FINISH_UNLOCKED),
                       (0x240, EVENT_ENABLE_STAMP), (0x248, EVENT_DISABLE_STAMP),
                       (0x258, "__ZN24IOAccelEventMachineFast226enableEventStampInterruptsEPK12IOAccelEvent"),
                       (0x260, "__ZN24IOAccelEventMachineFast227disableEventStampInterruptsEPK12IOAccelEvent")):
        raw = struct.unpack("<Q", read(address_of(EVENT_VTABLE) + 16 + slot, 8))[0]
        assert (raw >> 30) & 3 == 1 and raw >> 63 == 0, "unexpected cache level/auth"
        assert raw & 0x3fffffff == address_of(name), f"changed event virtual {slot:#x}"
    finish = read(address_of(EVENT_FINISH), 0x192)
    for call, target in ((0x14bb19aa, "__ZN18IOAccelDisplayPipe23disable_event_interruptEPK12IOAccelEvent"),
                         (0x14bb1aa6, "__ZN18IOAccelDisplayPipe22enable_event_interruptEPK12IOAccelEvent")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(target), "changed display gated queue reference transition"
    for address, encoded in ((0x14bb22a5, "48 8b 87 88 00 00 00"),
                            (0x14bb22ac, "48 8b b8 80 03 00 00"),
                            (0x14bb22b6, "ff 90 58 02 00 00"),
                            (0x14bb22fc, "48 8b 83 88 00 00 00"),
                            (0x14bb2303, "48 8b b8 80 03 00 00"),
                            (0x14bb2310, "ff 90 60 02 00 00")):
        expected = bytes.fromhex(encoded)
        assert read(address, len(expected)) == expected, "changed display-pipe event owner/enable-disable edge"
    assert hashlib.sha256(finish).hexdigest() == \
        "34c3638c2485ef35e2fdb1e70efeff61e935972b1db36bc72ec3084b7207c010", "changed finishEvent"
    assert hashlib.sha256(read(address_of(EVENT_SIGNAL), 0xa8)).hexdigest() == \
        "02c75b9f3864be75d75b3b2fd1b777a52e016265036c7e77c096d810db3e260b", "changed signalStamp"
    assert hashlib.sha256(read(address_of(EVENT_RESTART), 0x23e)).hexdigest() == \
        "35a836d5773e442205b5f415e658630d2e643ed364a39bb800312d48b424345c", "changed restart_channel"
    assert hashlib.sha256(read(address_of(EVENT_MERGE_EXCLUDING), 0x1d4)).hexdigest() == \
        "20517fccc05f6ca02f2e15867e6b55ef6ec21ab7872f14a5abd9410bd8b8b4d8", "changed mergeEventExcluding"
    assert read(address_of(EVENT_MERGE_EXCLUDING) + 0x1bb, 9) == bytes.fromhex("48 89 de ff 90 90 01 00 00"), "changed merged destination completion query (not merge-error status)"
    assert hashlib.sha256(read(address_of(EVENT_SET_STAMP), 0x9e)).hexdigest() == \
        "6240e1c9918181dd5d32c49d5fe01dab22e7705dc0d4d1d94b1e7189d8c8c995", "changed setEventStamp"
    assert read(address_of(EVENT_SET_STAMP) + 6, 4) == bytes.fromhex("48 8b 0c c2"), "changed direct event-entry read (no null admission guard)"
    wait = read(address_of(EVENT_WAIT), 0x34)
    assert hashlib.sha256(read(address_of(EVENT_WAIT), 0x314)).hexdigest() == \
        "66a79a6eeeae4a30fc91586b1e09f54b702ed2c86501af4e4d0e7bcfe7ddb1f9", "changed full waitForStamp"
    assert hashlib.sha256(read(address_of(EVENT_DISABLE_STAMP_LOCKED), 0x48)).hexdigest() == \
        "f1d296bd9cb53f41b56e43d5ad536695b5b4d83ef4ffa99cbd56a31837b021ea", "changed timeout waiter cleanup"
    for offset, encoded in ((0x5c, "42 89 14 a0"),
                            (0x6d, "ff 90 40 02 00 00"),
                            (0x202, "89 14 b0"),
                            (0x213, "ff 90 48 02 00 00"),
                            (0x287, "89 14 b0"),
                            (0x298, "ff 90 48 02 00 00")):
        expected = bytes.fromhex(encoded)
        assert read(address_of(EVENT_WAIT) + offset, len(expected)) == expected, "changed waiter reference/disable semantic anchor"
    cleanup_call = address_of(EVENT_WAIT) + 0x304
    encoded = read(cleanup_call, 5)
    assert encoded[0] == 0xe8 and cleanup_call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(EVENT_DISABLE_STAMP_LOCKED), "changed timeout waiter balanced cleanup call"
    finish_stamp = address_of("__ZN24IOAccelEventMachineFast211finishStampEi")
    assert read(finish_stamp + 0xb5, 6) == bytes.fromhex("ff 90 38 02 00 00"), "changed channel finish waitForStamp dispatch"
    assert wait[0x1c:0x32] == bytes.fromhex(
        "48 8b 4f 10 31 c0 83 b9 c8 0d 00 00 00 0f 85 f5 01 00 00 41 89 f7"), \
        "changed waitForStamp non-hardware early-success branch"
    assert read(address_of(EVENT_WAIT) + 0x224, 15) == bytes.fromhex(
        "48 83 c4 28 5b 41 5c 41 5d 41 5e 41 5f 5d c3"), "changed wait epilogue"
    print("PASS event virtuals, finishEvent identity and waitForStamp early-success path")
    if boot_path is not None:
        check_boot_atomic(image, boot_path)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: tahoe_ioaccel_mapping_contract_test.py SystemKernelExtensions.kc [BootKernelExtensions.kc]")
    check(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else None)

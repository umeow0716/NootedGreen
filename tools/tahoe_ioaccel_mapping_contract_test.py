#!/usr/bin/env python3
"""Check a locally archived Tahoe KC; not a GPU completion/safety test."""

import hashlib
import pathlib
import struct
import sys


KC_SHA256 = "5cb1be1dc530b4b953a33943567589101d3ac46bb8cf90728566ee7e5b1fa214"
BOOT_SHA256 = "5cba9e36ceed5d73e1d569d1772bc46fecbd0359f824db689863e686d856ea3b"
IDENTIFIER = b"com.apple.iokit.IOAcceleratorFamily2"
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
            if boot[start:boot.index(0, start)] == b"com.apple.kernel":
                kernel.append(file_offset)
    assert len(kernel) == len(bases) == 1, "missing/ambiguous kernel/base"
    symbols = {name: [] for name in (b"_OSIncrementAtomic", b"_OSDecrementAtomic", b"_thread_wakeup_prim",
                                   b"__ZN15IORegistryEntry18getRegistryEntryIDEv", b"_kernel_debug",
                                   b"_IOLockLock", b"_IOLockUnlock", b"_assert_wait_deadline",
                                   b"_thread_block", b"_clock_interval_to_deadline",
                                   b"__ZN10IOWorkLoop8workLoopEv",
                                   b"__ZN10IOWorkLoop14runActionBlockEU13block_pointerFivE",
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

    for name, length, digest in (
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
    symtab = None
    for command, offset in commands(image, entries[0]):
        if command == 0x19:
            fields = struct.unpack_from("<II16sQQQQIIII", image, offset)
            segments.append((fields[3], fields[5], fields[6]))
        elif command == 2:
            assert symtab is None, "duplicate symbol table"
            symtab = struct.unpack_from("<6I", image, offset)[2:]
    assert symtab is not None, "missing embedded symbol table"
    symbol_offset, count, string_offset, string_size = symtab
    matches = {name: [] for name in {*CONTRACTS, *SCRUB_BODIES, *LOCK_COPIES, *EVENT_OWNER_BODIES, SHARED_VTABLE, RESOURCE_VTABLE, "__ZTV18IOAccelDisplayPipe", "__ZTV24IOAccelLegacyDisplayPipe", "__ZTV16IOAccelMemoryMap", "__ZTV16IOAccelSysMemory", "__ZTV11IOAccelTask", "__ZTV24IOAccelSharedUserClient2",
                                    "__ZTV13IOAccelMemory", "__ZTV22IOGraphicsAccelerator2",
                                    "__ZN22IOGraphicsAccelerator223freeWaitToPrepareVidMapEP16IOAccelMemoryMapbb",
                                    "__ZNK16IOAccelMemoryMap9getLengthEv",
                                    EVENT_VTABLE, EVENT_FINISH, EVENT_WAIT, EVENT_CLEAN, EVENT_SIGNAL, EVENT_RESTART,
                                    EVENT_MERGE_EXCLUDING, EVENT_SET_STAMP, GET_DATA_BUFFER,
                                    EVENT_INIT, EVENT_COPY, EVENT_FINISH_UNLOCKED, EVENT_HARDWARE_ERROR,
                                    EVENT_DISABLE_STAMP_LOCKED, EVENT_ENABLE_STAMP, EVENT_DISABLE_STAMP}}
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

    for name, expected in CONTRACTS.items():
        address = address_of(name)
        assert read(address, len(expected)) == expected, f"changed {name}"
        print(f"PASS {name} at {address:#x}")
    for name, (length, digest) in SCRUB_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, f"changed {name}"
    for name, (length, digest) in EVENT_OWNER_BODIES.items():
        assert hashlib.sha256(read(address_of(name), length)).hexdigest() == digest, f"changed event owner lifecycle: {name}"
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
    for call, method in ((0x14b8e78d, "__ZN25IOAccelOrphanedMemoryPool13sharedReleaseEP14IOAccelShared2"),
                         (0x14b8e79e, "__ZN25IOAccelOrphanedMemoryPool13sharedReleaseEP14IOAccelShared2"),
                         (0x14b8e90d, "__ZN11IOAccelTask23prune_orphaned_mappingsEv")):
        encoded = read(call, 5)
        assert encoded[0] == 0xe8 and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(method), "changed Shared teardown cleanup edge"
    assert read(0x14b8e91c, 14) == bytes.fromhex("ff 50 28 48 c7 83 88 00 00 00 00 00 00 00"), "changed Shared task release/identity clear ordering"
    raw_shared_release = struct.unpack("<Q", read(address_of(RESOURCE_VTABLE) + 16 + 0x160, 8))[0]
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
                         (0x14bb06a3, "__ZN18IOAccelDisplayPipe17transaction_queueEP30IOAccelDisplayPipeTransaction2")):
        encoded = read(call, 5)
        assert encoded[0] in (0xe8, 0xe9) and call + 5 + struct.unpack_from("<i", encoded, 1)[0] == address_of(target), "changed transaction-end dispatch/queue edge"
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

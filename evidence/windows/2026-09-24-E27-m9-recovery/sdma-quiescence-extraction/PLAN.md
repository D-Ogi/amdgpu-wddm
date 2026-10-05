# SDMA queue quiescence extraction

Mechanically adapt v6.18sdma_v5_0_stop_queue registerbody as an uncalledshimprimitive. Caller mustown appropriate RLCsafe-mode scope,serializedengine/backing andlaterunfreeze/reprogramming. Keep ringdisable,FREEZErequest,FROZENpoll/STATUS1idlefallback,F32halt,UTC_L1disable order. Noreset,release orfirmwarereorder. Exportpairedunfreeze fragmentfromrestore_queue butdo notclaimthatfragmentalone restoresqueue. NoKMDcaller yet.
Testbothrealinstances with originalAMDmacroaddresses,ACK/idlefallback paths,exactwriteorder/masks/retainedbits,pendingfreeze onfailure,andpairedunfreeze. Preserveordinaryreplay andWDKshimcompile. HardwareandRLC-scopeintegration remainfuturework.

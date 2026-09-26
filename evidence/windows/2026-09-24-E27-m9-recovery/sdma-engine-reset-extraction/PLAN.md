# M372 - SDMA per-engine reset extraction plan

Hypothesis: the actual AMD per-engine reset callback can be imported without changing its register access and delay order, with caller ownership made explicit. This is a source/model experiment, not a hardware reset trial.

Reference Linux v6.18 commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449, amdgpu MIT: amdgpu_sdma_reset_engine, sdma_v5_0_stop_queue, sdma_v5_0_soft_reset_engine, sdma_v5_0_restore_queue and sdma_v5_0_gfx_resume_instance. Extract the callback mechanically, rename only interfaces/log/delay and add instance bounds. Keep it uncalled by KMD until complete reset/restoration composition is reviewed. Model both engines, unrelated bits, changed assertion readback, and verify R/W/readback/50us/release/readback order. Existing first-start replay must remain exact, WDK shim compilation must pass. A model cannot prove reset effectiveness or readiness on unit A.

Caller must serialize admission, retain all mappings/backing, quiesce queues with the RLC scope, perform reset and reestablish known halt/queue/cache state before retirement or restore addresses/translations before work. Upstream reset_engine ignores stop/start callback return values; Windows composition must require their success. No synthetic fence completion is imported. No hardware mutation in this extraction.

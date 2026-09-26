# Run007: node-by-node execution completes

Fresh boot02:56:03, KMD0.7.56.1. Elevated diagnostic callback fixes the log permissions problem. Both CPU/GPU traversals reach result_output and exit0. KMD536hardware submitted,536completed,0timeouts,0refusals,0UMDnotrun. All161 node callbacks complete. There are111 matched contiguous F32 destinations; all are finite. Maximum reported relative L2 error is0.001585820934, not an acceptance tolerance. Final512 logits choose token432 on bothCPU andGPU. Full text generation is not tested.

The callback synchronizes and prevents normal graph batching/fusion; warmup is also disabled by the upstream example. Therefore this does not validate ordinary inference. Next isolate no-warmup without callbacks.

Sources are sampled after execution: in-place operations can overwrite source data, so those samples are not necessarily pre-operation inputs. Raw selected tensors/logs are retained here; all raw per-node files remain in scratch/m9/ops-gpu-007, with hashes here. Display-only restored02:58:56, UnconfirmedStarts0.

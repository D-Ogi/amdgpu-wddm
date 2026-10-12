# Independent CPU and CUDA text controls

This addendum records controls received after the initial allocation-cap report on 2026-10-11.
The earlier report and its lab results remain unchanged.

PROVENANCE: llama.cpp, MIT, upstream release b11539 at 10a60cf30.

The model SHA-256 remains 93567E57A8FE10B23569B9D9EC38CD005DEEDF71E29477C421A4B83F418A538B.
Its file size is 6,975,879,296 bytes.
The reference runs use the CUDA 13.4 release on the development PC.
This review reads the existing outputs and does not run another workload.

The raw prompt is `The capital of France is`, with greedy sampling and a 16-token limit.
CUDA with automatic FA, CUDA with FA off and strict CPU each return `0111111111111111` and exit zero.
Strict CPU disables device selection, layer offload, operation offload and KV offload.
All three complete outputs match the retained HIP and Vulkan results after CRLF-to-LF normalization.
[oracle-controls.json](oracle-controls.json) holds the arguments, exit receipts, hashes and comparison rule.
The referenced stdout files retain their original bytes.

This replaces the earlier two-token CPU comparison as the complete reference for that raw prompt.
It does not establish general model accuracy or validate GPU Flash Attention on HIP.
The instruction model needs its chat template for the next user-facing text check.

The CUDA template arm uses `--jinja -cnv -st` and a 32-token limit.
Its prompt is `What is the capital of France? Answer in one word.`
The captured thought channel contains `The capital of France is Paris.`.
No final answer channel appears before the bound.
The last attempt exits zero. An earlier failed launch remains listed in the receipt.
The next lab arm must use the template and distinguish a final answer from thought text.

The raw-prompt controls remain historical regression evidence.
They do not replace the requested template test on HIP and Vulkan.
Full host stderr, model weights and binary packages remain outside the repository.
Only selected stdout and derived metadata are exported here.

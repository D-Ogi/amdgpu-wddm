from pathlib import Path
import hashlib, json, sys
source = Path(sys.argv[1])
output = Path(sys.argv[2])
text = source.read_text(encoding="utf-8")
start = text.index("static void sdma_v5_0_ring_emit_ib(")
opening = text.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (text[end] == "{") - (text[end] == "}")
    end += 1
body = text[start:end].replace("sdma_v5_0_ring_emit_ib", "reference_sdma_emit_ib", 1)
prefix = """/* PROVENANCE: imported AMD Linux v6.18 sdma_v5_0.c, MIT. */
struct amdgpu_job { unsigned int vmid; };
struct amdgpu_ib { u64 gpu_addr; u32 length_dw; };
static u64 reference_csa;
#define AMDGPU_JOB_GET_VMID(job) ((job)->vmid)
#define amdgpu_sdma_get_csa_mc_addr(ring, vmid) (reference_csa)
#define sdma_v5_0_ring_insert_nop amdgpu_ring_insert_nop
#pragma warning(push)
#pragma warning(disable:4100)
"""
suffix = """
#pragma warning(pop)
#undef AMDGPU_JOB_GET_VMID
#undef amdgpu_sdma_get_csa_mc_addr
#undef sdma_v5_0_ring_insert_nop
"""
output.write_text(prefix + body + suffix, encoding="utf-8")
output.with_suffix(".json").write_text(json.dumps({"source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "function": "sdma_v5_0_ring_emit_ib", "adaptation": "test job/IB types; caller-supplied CSA; single-word NOP mode; function name only changed"}, indent=2))

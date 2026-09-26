from pathlib import Path
p=Path('scratch/m13');s=Path('scratch/mesa-wddm2/src/gallium/frontends/d3d10umd/DxgiFns.cpp').read_text();a=s.index('HRESULT APIENTRY\n_RotateResourceIdentities');b=s.index('\n\n/*',a);fn=s[a:b]
pre=(p/'rotation-test-prelude.cpp').read_text();body=(p/'rotation-test-body.cpp').read_text()
(p/'rotation-test.cpp').write_text(pre+fn+body)
(p/'rotation-negative.cpp').write_text(pre+fn.replace('r->allocation = next.allocation;','/* mutation: omit kernel handle rotation */')+body)

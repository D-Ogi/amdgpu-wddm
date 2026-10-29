#!/usr/bin/env python3
"""Execute the pinned AMD scaling-update classifier with a minimal host context.

PROVENANCE: Linux amdgpu DC, MIT. The extracted function is unchanged.
This proves source behavior, not DCN buffering or Windows scheduling.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from mpo_transaction_model import body

PIN = '7d0a66e4bb9081d75c82ec4957c50034cb0ea449'

CONTEXT = r'''
enum surface_update_type { UPDATE_TYPE_FAST, UPDATE_TYPE_MED, UPDATE_TYPE_FULL };
struct rect { int x, y, width, height; };
struct quality { unsigned integer_scaling; };
union surface_update_flags {
    unsigned raw;
    struct { unsigned scaling_change:1, clock_change:1, bandwidth_change:1, position_change:1; } bits;
};
struct dc_plane_state {
    struct rect src_rect, dst_rect, clip_rect;
    struct quality scaling_quality;
    union surface_update_flags update_flags;
};
struct dc_scaling_info {
    struct rect src_rect, dst_rect, clip_rect;
    struct quality scaling_quality;
};
struct dc_surface_update { struct dc_plane_state *surface; struct dc_scaling_info *scaling_info; };
struct dc { struct { int max_optimizable_video_width; } caps; };
'''

TEST = r'''
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
static unsigned count;
#define CHECK(c) do { ++count; if (!(c)) ExitProcess(count); } while (0)
static struct dc_plane_state base(void) {
    struct dc_plane_state p = {{10,12,1920,1080},{20,22,1920,1080},{30,32,1920,1080},{0},{0}};
    return p;
}
static struct dc_scaling_info info(struct dc_plane_state p) {
    struct dc_scaling_info s = {p.src_rect,p.dst_rect,p.clip_rect,p.scaling_quality};
    return s;
}
void mainCRTStartup(void) {
    struct dc dc = {{4096}};
    struct dc_plane_state p = base();
    struct dc_scaling_info s = info(p);
    struct dc_surface_update u = {&p,&s};
    unsigned i;
    CHECK(get_scaling_info_update_type(&dc,&u)==UPDATE_TYPE_FAST);
    CHECK(!p.update_flags.raw);
    u.scaling_info=0;
    CHECK(get_scaling_info_update_type(&dc,&u)==UPDATE_TYPE_FAST);
    u.scaling_info=&s;
    for (i=0;i<6;++i) {
        p=base(); s=info(p);
        switch(i) {
        case 0:s.src_rect.x++;break; case 1:s.src_rect.y++;break;
        case 2:s.dst_rect.x++;break; case 3:s.dst_rect.y++;break;
        case 4:s.clip_rect.x++;break; default:s.clip_rect.y++;break;
        }
        CHECK(get_scaling_info_update_type(&dc,&u)==UPDATE_TYPE_MED);
        CHECK(p.update_flags.bits.position_change && !p.update_flags.bits.scaling_change);
    }
    for(i=0;i<7;++i) {
        p=base(); s=info(p);
        switch(i) {
        case 0:s.src_rect.width++;break; case 1:s.src_rect.height++;break;
        case 2:s.dst_rect.width--;break; case 3:s.dst_rect.height--;break;
        case 4:s.clip_rect.width--;break; case 5:s.clip_rect.height--;break;
        default:s.scaling_quality.integer_scaling=1;break;
        }
        CHECK(get_scaling_info_update_type(&dc,&u)==UPDATE_TYPE_FULL);
        CHECK(p.update_flags.bits.scaling_change);
        CHECK(p.update_flags.bits.clock_change==(i<2));
        CHECK(p.update_flags.bits.bandwidth_change==(i==2 || i==3));
    }
    // Equal aspect/ratio is insufficient: absolute dimensions still changed.
    p=base(); s=info(p); s.src_rect.width=960; s.dst_rect.width=960;
    CHECK(get_scaling_info_update_type(&dc,&u)==UPDATE_TYPE_FULL);
    // Clip growth requires extra bandwidth only above the strict size threshold.
    for(i=0;i<3;++i) {
        p=base(); p.src_rect.width=4095+(int)i; s=info(p); s.clip_rect.width++;
        CHECK(get_scaling_info_update_type(&dc,&u)==UPDATE_TYPE_FULL);
        CHECK(p.update_flags.bits.bandwidth_change==(i==2));
    }
    // Flags are accumulated: a simultaneous move must not downgrade a resize.
    p=base(); s=info(p); s.src_rect.x++; s.src_rect.width++;
    CHECK(get_scaling_info_update_type(&dc,&u)==UPDATE_TYPE_FULL);
    CHECK(p.update_flags.bits.position_change && p.update_flags.bits.scaling_change);
    CHECK(count==53);
    ExitProcess(0);
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--linux', type=Path, required=True)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    commit = subprocess.check_output(['git', '-C', str(args.linux), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != PIN:
        raise SystemExit('Review changed upstream revision: ' + commit)
    rels = ['core/dc.c', 'resource/dcn201/dcn201_resource.c', 'hwss/dcn201/dcn201_init.c',
            'hubp/dcn201/dcn201_hubp.c', 'optc/dcn201/dcn201_optc.c']
    prefix = 'drivers/gpu/drm/amd/display/dc/'
    raw = {r: (args.linux / prefix / r).read_bytes() for r in rels}
    src = {r: data.decode().replace('\r\n','\n') for r,data in raw.items()}
    # Bind the source files to the commit as well as the HEAD name.
    for r in rels:
        committed = subprocess.check_output(['git','-C',str(args.linux),'show',f'{PIN}:{prefix}{r}'])
        if committed.decode().replace('\r\n','\n') != src[r]:
            raise SystemExit('Modified reference: ' + r)
    core = src['core/dc.c']
    fast = re.findall(r'if\s*\(update_type == UPDATE_TYPE_FAST &&\s*'
                      r'dc->hwss.program_triplebuffer != NULL &&\s*'
                      r'!pipe_ctx->plane_state->flip_immediate && dc->debug.enable_tri_buf\)\s*'
                      r'\{\s*/\*triple buffer for VUpdate only\*/\s*'
                      r'pipe_ctx->plane_state->triplebuffer_flips = true;', core)
    checks = {
        'two_fast_non_immediate_gates': len(fast)==2,
        'full_update_clears_triple_flag': bool(re.search(r'if \(update_type == UPDATE_TYPE_FULL\)\s*\{[^}]*triplebuffer_flips = false;',core)),
        'non_fast_update_disables_triple': bool(re.search(r'if \(update_type == UPDATE_TYPE_FAST\)\s*continue;\s*ASSERT\(!pipe_ctx->plane_state->triplebuffer_flips\);',core)),
        'dcn201_default_enabled': '.enable_tri_buf = true' in src[rels[1]],
        'dcn201_program_callback': bool(re.search(r'\.program_triplebuffer\s*=\s*dcn20_program_triple_buffer',src[rels[2]])),
        'dcn201_enable_callback': bool(re.search(r'\.hubp_enable_tripleBuffer\s*=\s*hubp2_enable_triplebuffer',src[rels[3]])),
        'dcn201_triple_lock_uses_global_request': 'OTG_MASTER_UPDATE_LOCK, 1' in body(src[rels[4]],'optc201_triplebuffer_lock'),
    }
    if not all(checks.values()):
        raise SystemExit('Source contract changed: ' + repr(checks))
    fn = 'static enum surface_update_type get_scaling_info_update_type(const struct dc *dc, const struct dc_surface_update *u)\n{' + body(core,'get_scaling_info_update_type') + '}\n'
    args.out.mkdir(parents=True,exist_ok=True)
    c = args.out / 'amd-scaling-classifier.c'
    c.write_text(core[:core.index('*/')+2]+'\n'+CONTEXT+'\n'+fn+TEST,encoding='utf-8')
    compiler=args.root/'toolchain/llvm-23.1.4-win64/bin/clang-cl.exe'
    version=subprocess.check_output([str(compiler),'--version'],text=True)
    exe=args.out/'amd-scaling-classifier.exe'
    command=[str(compiler),'/nologo','/TC','/W4','/WX','/O2','/GS-','-fuse-ld=lld',
             '--target=x86_64-pc-windows-msvc',str(c),'/Fo'+str(args.out/'classifier.obj'),'/Fe'+str(exe),
             '/link','/nodefaultlib','/entry:mainCRTStartup','/subsystem:console',
             '/LIBPATH:'+str(args.root/'toolchain/nuget/microsoft.windows.sdk.cpp.x64/c/um/x64'),'kernel32.lib']
    subprocess.run(command,check=True,capture_output=True,text=True)
    run=subprocess.run([str(exe)],timeout=10)
    if run.returncode:
        raise SystemExit(f'AMD classifier failed check {run.returncode}')
    negative=[]
    # Mutate only the extracted function, not expected results or enum values.
    # The harness must reject both a lost resize promotion and a lost move promotion.
    for old in ('UPDATE_TYPE_FULL','UPDATE_TYPE_MED'):
        mutated=args.out/('wrong-'+old+'.c')
        mutated.write_text(core[:core.index('*/')+2]+'\n'+CONTEXT+'\n'+
                           fn.replace('return '+old+';', 'return UPDATE_TYPE_FAST;')+TEST,encoding='utf-8')
        wrong_exe=mutated.with_suffix('.exe')
        wrong_command=[str(mutated) if item==str(c) else
                       '/Fo'+str(mutated.with_suffix('.obj')) if item.startswith('/Fo') else
                       '/Fe'+str(wrong_exe) if item.startswith('/Fe') else item for item in command]
        subprocess.run(wrong_command,check=True,capture_output=True,text=True)
        wrong=subprocess.run([str(wrong_exe)],timeout=10)
        if not 1 <= wrong.returncode <= 53:
            raise SystemExit('Negative classifier control failed: '+old)
        negative.append({'mutation':old+' -> FAST','rejected_at_check':wrong.returncode})
    report={'linux_commit':commit,'compiler':version,'command':command,'source_checks':checks,
            'runtime_checks':53,'runtime_exit':run.returncode,'scope':'Unmodified AMD classifier in minimal context; not hardware, Windows DDI, or HLK',
            'negative_controls':negative,
            'sources_sha256':{r:hashlib.sha256(b).hexdigest() for r,b in raw.items()},
            'generated_source_sha256':hashlib.sha256(c.read_bytes()).hexdigest(),
            'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()}
    (args.out/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('AMD update classification: 7 source checks, 53 runtime checks, 2 mutation controls PASS; no hardware claim')


if __name__=='__main__':
    main()

"""No-SMU lifecycle checks of C096. Default is a plan; no device access."""
import argparse
import ctypes
import errno
import json
import mmap
import os
from pathlib import Path
import runpy
import select
import signal
import struct
import sys

DEVICE='/dev/bc250_dma_c096'
MODULE='dma_provider_C096'
CANARIES=(0xA55A3CC3,0x6DB6E719)
# Generated Linux x86_64 _IOC encodings, checked against C096.h by the ABI probe.
FILL=(1<<30)|(16<<16)|(ord('V')<<8)|0x60
EXPOSE=(3<<30)|(32<<16)|(ord('V')<<8)|0x61
SNAPSHOT=(3<<30)|(4112<<16)|(ord('V')<<8)|0x62
lib=None
checks=[]

def checked(name,condition):
    if not condition:raise RuntimeError('check failed: '+name)
    checks.append(name)

def request(slot,abi=1,reserved=0):
    return struct.pack('<IIII',abi,slot,reserved,0)

def raw_ioctl(fd,cmd,buffer):
    ctypes.set_errno(0)
    if lib.ioctl(fd,cmd,buffer)<0:
        code=ctypes.get_errno()
        raise OSError(code,os.strerror(code))

def call(fd,cmd,slot,size=16):
    b=ctypes.create_string_buffer(request(slot)+bytes(size-16),size)
    raw_ioctl(fd,cmd,ctypes.cast(b,ctypes.c_void_p))
    return b.raw

def rejects(name,expected,fn,unexpected_cleanup=None):
    try:value=fn()
    except OSError as exc:checked(name,exc.errno==expected)
    else:
        if unexpected_cleanup is not None:unexpected_cleanup(value)
        raise RuntimeError(name+' unexpectedly succeeded')

def snapshot(fd,slot):
    b=call(fd,SNAPSHOT,slot,4112)
    checked('snapshot request '+str(slot),b[:16]==request(slot))
    checked('full canary '+str(slot),b[16:]==struct.pack('<I',CANARIES[slot])*1024)

def refcount():
    return int((Path('/sys/module')/MODULE/'refcnt').read_text())

def open_device():return os.open(DEVICE,os.O_RDWR|os.O_CLOEXEC)

def unexposed():
    fd=open_device()
    try:
        rejects('exclusive open',errno.EBUSY,open_device,os.close)
        rejects('unknown ioctl',errno.ENOTTY,lambda:call(fd,FILL^1,0))
        rejects('wrong slot',errno.EINVAL,lambda:call(fd,FILL,2))
        for label,payload in [('wrong ABI',request(0,abi=2)),('reserved',request(0,reserved=1))]:
            b=ctypes.create_string_buffer(payload,16)
            rejects(label,errno.EINVAL,lambda:raw_ioctl(fd,FILL,ctypes.cast(b,ctypes.c_void_p)))
        rejects('null input',errno.EFAULT,lambda:raw_ioctl(fd,FILL,None))
        rejects('snapshot before fill',errno.EPERM,lambda:call(fd,SNAPSHOT,0,4112))
        rejects('expose before fill',errno.EPERM,lambda:call(fd,EXPOSE,0,32))
        call(fd,FILL,0)
        rejects('repeat fill0',errno.EPERM,lambda:call(fd,FILL,0))
        rejects('expose before both fills',errno.EPERM,lambda:call(fd,EXPOSE,0,32))
        snapshot(fd,0)
        call(fd,FILL,1)
        snapshot(fd,1)
    finally:os.close(fd)
    checked('no permanent reference before exposure',refcount()==0)
    fd=open_device()
    try:snapshot(fd,0);snapshot(fd,1)
    finally:os.close(fd)
    checked('close preserves unexposed state without self ref',refcount()==0)
    return dict(passed=True,phase='unexposed',checks=checks,exposed=False,no_smu=True)

def exposure_child(write_fd):
    try:
        fd=open_device()
        call(fd,FILL,0);call(fd,FILL,1)
        # Readable input plus unwritable output makes copy_to_user fail AFTER
        # the kernel latch. No DMA command or arbitrary address is issued.
        page=mmap.mmap(-1,4096,flags=mmap.MAP_PRIVATE|mmap.MAP_ANONYMOUS,
                       prot=mmap.PROT_READ|mmap.PROT_WRITE)
        page[:16]=request(0)
        va=ctypes.addressof(ctypes.c_char.from_buffer(page))
        checked('readonly copyout page',lib.mprotect(va,4096,mmap.PROT_READ)==0)
        rejects('expose copyout EFAULT',errno.EFAULT,
                lambda:raw_ioctl(fd,EXPOSE,ctypes.c_void_p(va)))
        rejects('copyout failure does not undo exposure',errno.EPERM,lambda:call(fd,EXPOSE,0,32))
        for slot in (0,1):
            rejects('fill denied after exposure '+str(slot),errno.EPERM,lambda:call(fd,FILL,slot))
            snapshot(fd,slot)
        descriptor=call(fd,EXPOSE,1,32)
        dma,size,canary=struct.unpack_from('<QII',descriptor,16)
        checked('descriptor input preserved',descriptor[:16]==request(1))
        checked('descriptor page',0<dma<=0xFFFFFFFF-4095 and dma%4096==0 and size==4096 and canary==CANARIES[1])
        checked('open plus retained reference',refcount()==2)
        message=json.dumps(dict(ready=True,checks=checks,dma_slot1=dma)).encode()
        checked('atomic child message',len(message)<4096)
        os.write(write_fd,message)
        os.close(write_fd)
        while True:signal.pause()
    except BaseException as exc:
        message=json.dumps(dict(ready=False,error=type(exc).__name__+': '+str(exc),checks=checks)).encode()
        try:os.write(write_fd,message[:4000])
        except OSError:pass
        os._exit(2)

def exposed():
    read_fd,write_fd=os.pipe()
    parent=os.getpid()
    pid=os.fork()
    if pid==0:
        arm_parent_death(parent)
        os.close(read_fd);exposure_child(write_fd);os._exit(2)
    os.close(write_fd)
    waited=False
    try:
        checked('child ready within bound',bool(select.select([read_fd],[],[],15)[0]))
        msg=json.loads(os.read(read_fd,4096))
        if msg.get('ready') is not True:raise RuntimeError('child exposure checks: '+json.dumps(msg))
        checked('child exposure checks',True)
        rejects('child owns exclusive file',errno.EBUSY,open_device,os.close)
        os.kill(pid,signal.SIGKILL)
        child,status=os.waitpid(pid,0);waited=True
        checked('actual child SIGKILL',child==pid and os.WIFSIGNALED(status) and os.WTERMSIG(status)==signal.SIGKILL)
        checked('kernel reference survives SIGKILL',refcount()==1)
        fd=open_device()
        try:
            for slot in (0,1):
                snapshot(fd,slot)
                rejects('refill remains denied '+str(slot),errno.EPERM,lambda:call(fd,FILL,slot))
                rejects('repeat exposure denied '+str(slot),errno.EPERM,lambda:call(fd,EXPOSE,slot,32))
        finally:os.close(fd)
        checked('kernel reference survives reopen/close',refcount()==1)
        # O_NONBLOCK only, NEVER O_TRUNC (force). This test must be refused.
        ctypes.set_errno(0)
        rc=lib.delete_module(MODULE.encode(),os.O_NONBLOCK)
        checked('ordinary module removal refused',rc==-1 and ctypes.get_errno()==errno.EWOULDBLOCK)
        checked('allocation owner remains loaded',refcount()==1)
        return dict(passed=True,phase='exposed',checks=checks,child=msg,
                    retained=True,no_smu=True,reset_required=True)
    finally:
        os.close(read_fd)
        if not waited:
            try:os.kill(pid,signal.SIGKILL)
            except ProcessLookupError:pass
            os.waitpid(pid,0)

def main():
    global lib,arm_parent_death
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--phase',choices=['unexposed','exposed'],required=True)
    p.add_argument('--execute',action='store_true')
    a=p.parse_args()
    if not a.execute:
        print('PLAN: C096 allocation/lifetime only, no SMU/MMIO/DMA submit; exposure requires cold reset.')
        return 0
    if sys.platform!='linux' or os.uname().machine!='x86_64':raise RuntimeError('requires Linux x86_64')
    if ctypes.sizeof(ctypes.c_void_p)!=8:raise RuntimeError('requires 64-bit userspace ABI')
    arm_parent_death=runpy.run_path(str(Path(__file__).with_name('dma_process_C099.py')))['arm_parent_death']
    lib=ctypes.CDLL(None,use_errno=True)
    lib.ioctl.argtypes=[ctypes.c_int,ctypes.c_ulong,ctypes.c_void_p];lib.ioctl.restype=ctypes.c_int
    lib.mprotect.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int];lib.mprotect.restype=ctypes.c_int
    lib.delete_module.argtypes=[ctypes.c_char_p,ctypes.c_uint];lib.delete_module.restype=ctypes.c_int
    result=unexposed() if a.phase=='unexposed' else exposed()
    print(json.dumps(result),flush=True)
    return 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except Exception as exc:
        print(json.dumps(dict(passed=False,error=type(exc).__name__+': '+str(exc),checks=checks,
                             no_smu=True,do_not_force_unload=True,reset_required=True)),flush=True)
        raise SystemExit(2)

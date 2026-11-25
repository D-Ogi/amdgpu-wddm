"""Real Linux process fault tests, no device, module, MMIO or SMU access."""
import argparse
import json
import os
from pathlib import Path
import runpy
import select
import signal
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
H=runpy.run_path(str(HERE/'dma_process_C099.py'))


def wait_for(predicate, seconds=5):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if predicate():return
        time.sleep(0.02)
    raise RuntimeError('test deadline')


def worker(mode, directory):
    parent=os.getpid()
    child=os.fork()
    if child==0:
        # Independent escape hatch even if the test harness fails before it
        # obtains pidfds. This is never the expected termination in a test.
        signal.alarm(30)
        if mode!='leader-exit':H['arm_parent_death'](parent)
        (directory/'child-ready').write_text(str(os.getpid()))
        while True:signal.pause()
    wait_for((directory/'child-ready').exists)
    temporary=directory/'ready.tmp'
    temporary.write_text(json.dumps([os.getpid(),child]))
    os.replace(temporary,directory/'ready')
    if mode=='leader-exit':
        wait_for((directory/'release').exists)
        os._exit(7)  # deliberately leave an unarmed descendant in this group
    while True:signal.pause()


def supervisor(mode, directory):
    stop=H['StopLatch']();stop.install()
    try:
        with (directory/'worker.out').open('xb') as out,(directory/'worker.err').open('xb') as err:
            rc=H['run_owned']([sys.executable,'-I','-B',__file__,'worker',mode,str(directory)],
                              out,err,stop.check,1 if mode=='deadline' else 10)
        stop.check()
        if rc:raise RuntimeError('worker exit '+str(rc))
        return 0
    except Exception as exc:
        (directory/'stopped').write_text(str(exc))
        return 2


def unit_checks(directory):
    passed=[]
    def refuses(label,fn):
        try:fn()
        except (RuntimeError,ValueError,OSError):passed.append(label)
        else:raise RuntimeError('did not refuse: '+label)
    for starting in (False,True):
        for value in (-1,97000,'invalid'):
            g=H['TrialGuard'](H['StopLatch'](),lambda:value)
            refuses('invalid sensor '+str((starting,value)),lambda:g.check(starting=starting))
    g=H['TrialGuard'](H['StopLatch'](),lambda:87000)
    refuses('87C start',lambda:g.check(starting=True))
    clock=[0];temp=[92000]
    g=H['TrialGuard'](H['StopLatch'](),lambda:temp[0],lambda:clock[0])
    g.check();clock[0]=9.99;g.check();clock[0]=10
    refuses('92C held10s',g.check)
    clock[0]=0
    g=H['TrialGuard'](H['StopLatch'](),lambda:temp[0],lambda:clock[0])
    g.check();clock[0]=9;temp[0]=91000;g.check()
    clock[0]=10;temp[0]=92000;g.check();clock[0]=19;g.check()
    clock[0]=20;refuses('92C timer reset',g.check)
    clock[0]=0
    g=H['TrialGuard'](H['StopLatch'](),lambda:73000,lambda:clock[0])
    clock[0]=90;refuses('whole90s bound',g.check)
    stop=H['StopLatch']();stop.receive(signal.SIGHUP,None);stop.receive(signal.SIGTERM,None)
    if stop.signum!=signal.SIGHUP:raise RuntimeError('STOP latch overwritten')
    marker=directory/'forbidden-launch'
    with open(os.devnull,'wb') as out:
        try:
            H['run_owned']([sys.executable,'-I','-c',
                            'from pathlib import Path; Path('+repr(str(marker))+').touch()'],
                           out,out,stop.check,1)
        except RuntimeError as exc:
            if not str(exc).startswith('STOP signal'):raise
        else:raise RuntimeError('STOP did not refuse launch')
    if marker.exists():raise RuntimeError('command ran despite STOP')
    passed.append('STOP forbids valid launch; marker absent')
    return passed


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('role',choices=['test','supervisor','worker','wrong-parent'])
    p.add_argument('mode');p.add_argument('directory',type=Path)
    a=p.parse_args()
    if a.role=='worker':worker(a.mode,a.directory);return 3
    if a.role=='supervisor':return supervisor(a.mode,a.directory)
    if a.role=='wrong-parent':H['arm_parent_death'](-1);return 3
    a.directory.mkdir()  # new evidence directory, no retry/overwrite
    passed=unit_checks(a.directory)
    for mode,signum in [('term',signal.SIGTERM),('hup',signal.SIGHUP),
                        ('kill',signal.SIGKILL),('leader-exit',None),('deadline',None)]:
        d=a.directory/mode;d.mkdir()
        with (d/'supervisor.out').open('xb') as out,(d/'supervisor.err').open('xb') as err:
            proc=subprocess.Popen([sys.executable,'-I','-B',__file__,'supervisor',mode,str(d)],
                                   stdin=subprocess.DEVNULL,stdout=out,stderr=err)
            pidfds=[]
            try:
                wait_for(lambda:(d/'ready').exists() and (d/'ready').stat().st_size>0)
                # Each descriptor refers to this process, even if its PID is reused.
                for pid in json.loads((d/'ready').read_text()):pidfds.append(os.pidfd_open(pid))
                if signum is not None:proc.send_signal(signum)
                if mode=='leader-exit':(d/'release').write_text('exit')
                rc=proc.wait(timeout=5)
                if rc!=(-signal.SIGKILL if mode=='kill' else 2):
                    raise RuntimeError('unexpected supervisor exit '+str((mode,rc)))
                for fd in pidfds:
                    if not select.select([fd],[],[],3)[0]:raise RuntimeError('surviving descendant '+mode)
                if mode!='kill':
                    error=(d/'stopped').read_text()
                    expected={'term':'STOP signal','hup':'STOP signal','leader-exit':'worker exit 7',
                              'deadline':'owned process deadline'}[mode]
                    if expected not in error:raise RuntimeError('wrong stop reason: '+error)
                passed.append(mode+' stops worker and grandchild')
            finally:
                if proc.poll() is None:proc.kill();proc.wait(timeout=3)
                for fd in pidfds:
                    if not select.select([fd],[],[],0)[0]:
                        signal.pidfd_send_signal(fd,signal.SIGKILL)
                    os.close(fd)
    rc=subprocess.run([sys.executable,'-I','-B',__file__,'wrong-parent','none',str(a.directory)],
                      timeout=3,check=False).returncode
    if rc!=125:raise RuntimeError('parent-death setup race not refused')
    passed.append('parent identity mismatch takes exit125 branch')
    print(json.dumps(dict(passed=True,no_hardware=True,checks=passed)),flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())

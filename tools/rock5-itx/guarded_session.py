#!/usr/bin/env python3
"""Run a native session with an explicitly scoped NanoKVM hardware watchdog."""
import contextlib
import getpass
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time
import urllib.error

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import lab
import nanokvm
from controller_watchdog import ControllerWatchdog

os.umask(0o077)
config=json.loads(lab.CONFIG.read_text())
nanokvm.BASE=config['nanokvm_url']
if len(sys.argv) == 1 or '-h' in sys.argv or '--help' in sys.argv:
    sys.argv[0] = str(TOOLS / 'session.py')
    runpy.run_path(sys.argv[0], run_name='__main__')
    raise SystemExit(0)
web_password = os.environ.get('NANOKVM_PASSWORD') or getpass.getpass(
    'NanoKVM web password for automatic recovery: ')

def login():
    os.environ['NANOKVM_PASSWORD']=web_password
    try:
        deadline=time.monotonic()+75
        while True:
            try:
                nanokvm.login('admin')
                nanokvm.api('/api/vm/hardware')
                return
            except urllib.error.HTTPError as error:
                if error.code not in (502,503,504):raise
                if time.monotonic()>=deadline:raise
            except (urllib.error.URLError,TimeoutError,ConnectionError):
                if time.monotonic()>=deadline:raise
            time.sleep(3)
    finally:os.environ.pop('NANOKVM_PASSWORD',None)

login()
output=lab.WORK/'artifacts/controller-guarded-native'/lab.timestamp()
output.mkdir(parents=True)
print(json.dumps({'controller_guard':str(output)}),flush=True)
original_lock=lab.lock
original_recover=lab.recover
original_start_serial=lab.start_serial
active={}

@contextlib.contextmanager
def guarded_lock(name):
    with original_lock(name):
        if name!='hardware':
            yield
            return
        with ControllerWatchdog(config,output) as guard:
            active['guard']=guard
            yield


def start_serial(current,path):
    active['serial_path']=path
    return original_start_serial(current,path)


def controller_id(current):
    try:return lab.ssh(current,'nanokvm_ssh','cat /proc/sys/kernel/random/boot_id',timeout=6)
    except (subprocess.SubprocessError,OSError):return None


def recover(current):
    guard=active['guard']
    before=guard.receipt['ready']['boot_id']
    observed=controller_id(current)
    need_restart=bool(guard.error) or observed is None or observed!=before
    if not need_restart:return original_recover(current)
    report={'status':'incomplete','controller_boot_before':before}
    print(json.dumps({'waiting_for_controller_watchdog':True}),flush=True)
    serial=None
    try:
        deadline=time.monotonic()+150
        while time.monotonic()<deadline:
            if observed and observed!=before:break
            time.sleep(4)
            observed=controller_id(current)
        else:raise RuntimeError('Controller watchdog did not restore SSH with a new boot ID')
        report['controller_boot_after']=observed
        login()
        serial=original_start_serial(current,output/'serial-controller-recovery.log')
        value=original_recover(current)
        report.update(status='pass',recovery=value)
        return value
    except BaseException as error:
        report.update(status='error',error=str(error))
        raise
    finally:
        if serial:
            try:report['serial']=serial.stop()
            except BaseException as error:report['serial_error']=str(error)
        lab.save(output/'controller-recovery.json',report)

lab.lock=guarded_lock
lab.recover=recover
lab.start_serial=start_serial
sys.argv[0] = str(TOOLS / 'session.py')
runpy.run_path(sys.argv[0],run_name='__main__')

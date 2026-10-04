"""Scoped NanoKVM hardware watchdog guard; the caller must own the hardware lock."""
import json
import select
import shlex
import subprocess
import threading
import time

REMOTE = r'''
import array,fcntl,json,os,select,signal,subprocess,sys,time
from pathlib import Path
signal.signal(signal.SIGHUP,signal.SIG_IGN)
def integer(fd,number,value=0,direction=2):
    data=array.array('i',[value])
    fcntl.ioctl(fd,(direction<<30)|(4<<16)|(ord('W')<<8)|number,data,True)
    return data[0]
def emit(value):
    print(json.dumps(value),flush=True)
assert Path('/sys/module/soph_wdt/parameters/nowayout').read_text().strip()=='N'
fd=os.open('/dev/watchdog0',os.O_WRONLY|os.O_CLOEXEC)
try:
    os.write(fd,b'.')
    reported=integer(fd,6,30,3)
    os.write(fd,b'.')
    emit({'ready':True,'requested_timeout':30,'reported_timeout':reported,
          'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()})
except BaseException:
    integer(fd,4,1);os.write(fd,b'V');os.close(fd)
    raise
pending=b''
while True:
    ready,_,_=select.select([0],[],[],12)
    data=os.read(0,256) if ready else b''
    if not data:
        # Deliberately keep the watchdog armed after lost control.
        # No startup service is installed; a reboot ends the guard.
        report={'event':'heartbeat_lost','utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                'uptime':Path('/proc/uptime').read_text(),
                'meminfo':Path('/proc/meminfo').read_text(),
                'netdev':Path('/proc/net/dev').read_text()}
        try:
            report['dmesg']=subprocess.run(['dmesg'],capture_output=True,text=True,timeout=3).stdout
            with open(DIAGNOSTIC,'x') as log:
                json.dump(report,log);log.flush();os.fsync(log.fileno())
        except BaseException:pass
        os._exit(73)
    pending+=data
    if len(pending)>256:os._exit(74)
    while b'\n' in pending:
        line,pending=pending.split(b'\n',1)
        if line==b'stop':
            integer(fd,4,1);os.write(fd,b'V');os.close(fd)
            emit({'disarmed':True})
            sys.exit(0)
        if not line.startswith(b'ping ') or not line[5:].isdigit():os._exit(75)
        os.write(fd,b'.')
        emit({'pong':int(line[5:])})
'''


class ControllerWatchdog:
    def __init__(self,config,output):
        self.config=config
        self.output=output
        self.process=None
        self.error=None
        self.stop_event=threading.Event()
        self.thread=None
        self.buffer=b''
        self.receipt={'status':'incomplete'}

    def read(self,timeout=8):
        deadline=time.monotonic()+timeout
        while b'\n' not in self.buffer:
            remaining=deadline-time.monotonic()
            if remaining<=0 or not select.select([self.process.stdout],[],[],remaining)[0]:
                raise TimeoutError('Watchdog heartbeat acknowledgement timed out')
            import os
            data=os.read(self.process.stdout.fileno(),4096)
            if not data:raise EOFError('Watchdog SSH stream ended')
            self.buffer+=data
        line,self.buffer=self.buffer.split(b'\n',1)
        value=json.loads(line)
        self.log.write(json.dumps(value)+'\n');self.log.flush()
        return value

    def write(self,line):
        self.process.stdin.write(line.encode()+b'\n')
        self.process.stdin.flush()

    def heartbeat(self):
        sequence=0
        try:
            while not self.stop_event.wait(2):
                self.write('ping '+str(sequence))
                if self.read()!={'pong':sequence}:
                    raise RuntimeError('Unexpected watchdog heartbeat response')
                sequence+=1
        except BaseException as error:
            self.error=error
            self.receipt['heartbeat_error']=str(error)

    def __enter__(self):
        self.output.mkdir(parents=True,exist_ok=True)
        self.log=(self.output/'watchdog.jsonl').open('w')
        self.errors=(self.output/'watchdog-ssh.log').open('w')
        self.receipt['diagnostic']='/data/haiku-watchdog-'+self.output.name+'.json'
        code='DIAGNOSTIC='+repr(self.receipt['diagnostic'])+'\n'+REMOTE
        command=['ssh','-F',self.config['ssh_config'],'-o','ServerAliveInterval=3',
                 '-o','ServerAliveCountMax=2',self.config['nanokvm_ssh'],
                 'python3 -u -c '+shlex.quote(code)]
        self.process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.errors)
        try:
            self.receipt['ready']=self.read(15)
            if not self.receipt['ready'].get('ready'):raise RuntimeError('Watchdog failed to arm')
            self.thread=threading.Thread(target=self.heartbeat,daemon=True)
            self.thread.start()
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        self.stop_event.set()
        if self.thread:self.thread.join(timeout=10)
        try:
            if self.process and self.process.poll() is None and self.error is None:
                self.write('stop')
                if self.read()!={'disarmed':True}:raise RuntimeError('No watchdog disarm acknowledgement')
                self.process.stdin.close()
                self.process.wait(timeout=8)
                if self.process.returncode:raise RuntimeError('Watchdog disarm worker failed')
                self.receipt['status']='disarmed'
            else:
                self.receipt['status']='control_lost_watchdog_left_armed'
        except BaseException as error:
            self.error=error
            self.receipt.update(status='error',error=str(error))
        finally:
            if self.process:
                if self.process.poll() is None:
                    self.process.terminate()
                    try:self.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=5)
                for stream in (self.process.stdin,self.process.stdout):
                    if stream and not stream.closed:stream.close()
            self.log.close();self.errors.close()
            (self.output/'watchdog-result.json').write_text(json.dumps(self.receipt,indent=2)+'\n')

    def __exit__(self,kind,value,traceback):
        self.close()
        if self.error is not None and kind is None:raise self.error

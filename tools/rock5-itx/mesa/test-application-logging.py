"""Synthetic capture-contract tests; no application or GPU is executed."""
import hashlib
import json
from pathlib import Path
import re
import application_logging_validation as check

def block(stream, data):
    return (f'ROCK5_APPLICATION_LOG_BEGIN stream={stream} bytes={len(data.encode())} sha256={hashlib.sha256(data.encode()).hexdigest()}\n'
        +data+f'ROCK5_APPLICATION_LOG_END stream={stream}\n')

def fixture(software=False):
    controller='ROCK5_APPLICATION_MENU item=Filled polygons marked=0\n'
    renderer='' if software else 'HAIKU_MESA_NATIVE_GPU synthetic\nHAIKU_PAN_SW_POLYGON synthetic\n'
    return ''.join(f'ROCK5_APPLICATION_RUN_BEGIN cycle={i} mode=--'+('software' if software else 'native')+'\n'
        +block('controller',controller)+block('renderer',renderer)
        +f'ROCK5_APPLICATION_RUN_EXIT cycle={i} application=0 frames=0\n' for i in range(2))

original=fixture();assert len(check.validate(original)['streams'])==4
assert len(check.validate(fixture(True),software=True)['streams'])==4
unicode=original.replace(block('controller','ROCK5_APPLICATION_MENU item=Filled polygons marked=0\n'),
    block('controller','ROCK5_APPLICATION_MENU item=Filled polygons marked=0\nUTF-8 µ\n'),1)
check.validate(unicode)
tests=[]
def reject(name,text,software=False):
    assert text!=original
    try:check.validate(text,software=software)
    except (AssertionError,ValueError):tests.append(name)
    else:raise AssertionError('Accepted '+name)

for name,pattern,replacement in (
    ('wrong_size',r'bytes=\d+','bytes=1'),('wrong_hash',r'sha256=[0-9a-f]{64}','sha256='+'0'*64),
    ('wrong_stream',r'LOG_BEGIN stream=controller','LOG_BEGIN stream=renderer'),
    ('wrong_footer',r'LOG_END stream=controller','LOG_END stream=renderer'),
    ('missing_header',r'^ROCK5_APPLICATION_LOG_BEGIN .*\n',''),
    ('missing_footer',r'^ROCK5_APPLICATION_LOG_END .*\n',''),
    ('damaged_payload',r'marked=0','marked=1'),
    ('wrong_run_mode',r'mode=--native','mode=--software'),
    ('failed_process',r'application=0','application=1')):
    text,n=re.subn(pattern,replacement,original,count=1,flags=re.M);assert n==1
    reject(name,text)
reject('duplicate_stream',original+block('renderer',''))
for name,stream,data in (
    ('gpu_in_controller','controller','HAIKU_MESA_NATIVE_GPU synthetic\n'),
    ('menu_in_renderer','renderer','ROCK5_APPLICATION_MENU item=Filled polygons marked=0\n'),
    ('unterminated_payload','controller','ROCK5_APPLICATION_MENU item=Filled polygons marked=0')):
    text,n=re.subn(r'^ROCK5_APPLICATION_LOG_BEGIN stream='+stream+r'.*?^ROCK5_APPLICATION_LOG_END stream='+stream+r'\n',
        lambda _:block(stream,data),original,count=1,flags=re.M|re.S);assert n==1
    reject(name,text)
text=fixture(True).replace(block('renderer',''),block('renderer','HAIKU_MESA_NATIVE_GPU synthetic\n'),1)
reject('software_claims_native',text,software=True)
result=dict(status='pass',synthetic=True,native_executed=False,rejected=len(tests),tests=tests,
    validator_sha256=hashlib.sha256(Path(check.__file__).read_bytes()).hexdigest())
Path(__file__).with_name('application-logging-tests.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))

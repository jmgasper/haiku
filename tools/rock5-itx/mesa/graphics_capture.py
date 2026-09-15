"""Run bounded graphics fixtures using the qualified RAM capture protocol."""
from sustained_capture import CHUNK, RAM, PREFIX, manifest, assemble, cleanup
import sustained_capture

FIXTURES = {
    'window': ('run-window', 'ROCK5_WINDOW_INVENTORY_EXIT'),
    'glview': ('run-glview', 'ROCK5_GLVIEW_INVENTORY_EXIT'),
    'pipeline': ('run-pipeline', 'ROCK5_PIPELINE_INVENTORY_EXIT'),
    'lifetime': ('run-lifetime', 'ROCK5_LIFETIME_INVENTORY_EXIT'),
    'pressure': ('run-heap-pressure', 'ROCK5_PRESSURE_INVENTORY_EXIT'),
    'limit': ('run-heap-limit', 'ROCK5_LIMIT_INVENTORY_EXIT'),
    'concurrency': ('run-concurrency', 'ROCK5_CONCURRENT_INVENTORY_EXIT'),
    'pending': ('run-pending', 'ROCK5_PENDING_INVENTORY_EXIT'),
    'recovery': ('run-recovery', 'ROCK5_RECOVERY_INVENTORY_EXIT'),
}

def script(kind, software=False):
    launcher, marker = FIXTURES[kind]
    return sustained_capture.script(software).replace(
        '/boot/home/mesa-trial/run-sustained ', '/boot/home/mesa-trial/'+launcher+' ').replace(
        'ROCK5_SUSTAIN_INVENTORY_EXIT', marker)

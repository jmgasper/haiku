"""Check a system-launch GLTeapot transcript: no launch environment, installed defaults only.

The launcher clears every library, vendor, polygon, device and firmware
selection, so what renders is what the image installs system-wide. The
probe records whether the Mali device was present; with it absent (the
emulator) the default must have fallen back to software rendering. The
frame, process and logging checks are the application fixture's own.
"""
import re
import application_logging_validation
import application_validation

LIBRARIES = '/boot/system/non-packaged/lib'
VENDOR_DIRECTORY = '/boot/system/non-packaged/add-ons/opengl/egl_vendor.d'


def validate(text, output=None, device='present'):
    assert device in ('present', 'absent')
    software = device == 'absent'
    result = application_validation.validate(text, output, software=software, mode='--system')
    lines = re.findall(r'^ROCK5_APPLICATION_SYSTEM device=(present|absent) libraries=(\S+) vendor=(\S+)$',
        text, re.M)
    assert len(lines) == 2, lines
    assert all(line == (device, LIBRARIES, VENDOR_DIRECTORY) for line in lines), lines
    # The launcher leaves nothing selected: none of the overrides may appear in the transcript.
    for name in ('LIBRARY_PATH=', '__EGL_VENDOR_LIBRARY_FILENAMES=', 'HAIKU_CSF_DEVICE=',
            'HAIKU_CSF_FIRMWARE=', 'HAIKU_CSF_TRACE=', 'HAIKU_PAN_SW_POLYGON=', 'GALLIUM_DRIVER=',
            'LIBGL_ALWAYS_SOFTWARE='):
        assert name not in text, name
    logging = application_logging_validation.validate(text, software=software, mode='--system')
    return dict(result, launch='system', device=device, libraries=LIBRARIES,
        vendor_directory=VENDOR_DIRECTORY, logging=logging)

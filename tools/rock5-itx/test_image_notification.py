"""Check actual image notifications with Haiku's production message container."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ImageNotificationTests(unittest.TestCase):
    def test_pointer_payload_on_64_bit_host(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        image = (source / 'src/system/kernel/image.cpp').read_text()
        begin = image.index('class ImageNotificationService :')
        end = image.index('\n} // namespace', begin)
        includes = [
            'headers/build', 'headers/build/os', 'headers/build/os/app',
            'headers/build/os/drivers', 'headers/build/os/kernel',
            'headers/build/os/interface', 'headers/build/os/storage',
            'headers/build/os/support', 'headers/build/private/kernel',
            'headers/build/private/libroot', 'headers/build/private/system',
            'headers/private/system',
        ]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'image_notification.inc').write_text(image[begin:end])
            binary = root / 'image-notification-test'
            command = ['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-multichar', '-Wno-unused-parameter', '-Wno-ignored-qualifiers',
                '-fsanitize=address,undefined',
                '-fno-sanitize-recover=all', '-DKMESSAGE_CONTAINER_ONLY=1',
                '-DHAIKU_HOST_PLATFORM_LINUX=1', '-include', 'BeOSBuildCompatibility.h',
                '-include', 'initializer_list', '-I', str(root)]
            for include in includes:
                command += ['-I', str(source / include)]
            command += [str(directory / 'test_image_notification.cpp'),
                str(source / 'src/system/kernel/messaging/KMessage.cpp'), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('64-bit add/remove delivery and old-capacity rejection passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

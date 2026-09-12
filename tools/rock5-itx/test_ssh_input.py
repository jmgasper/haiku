"""Exercise subprocess input isolation with a real stdin-consuming SSH fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import lab


class SSHInputTests(unittest.TestCase):
    def run_fixture(self, mode):
        with tempfile.TemporaryDirectory(dir=lab.WORK / 'tmp') as directory:
            root = Path(directory)
            executable = root / 'ssh'
            executable.write_text('#!' + sys.executable + '\n'
                                  'import json,sys\n'
                                  'print(json.dumps(sys.stdin.read()))\n')
            executable.chmod(0o700)
            program = '''import json,sys
sys.path.insert(0, sys.argv[1])
import lab
config = {'ssh_config': sys.argv[2], 'target': 'local-fixture'}
mode = sys.argv[3]
if mode == 'legacy':
    received = lab.run(['ssh', 'local-fixture'])
else:
    value = 'sudo-fixture\\n' if mode == 'explicit' else None
    received = lab.ssh(config, 'target', 'fixture-command', input=value)
print(json.dumps({'child': json.loads(received), 'remaining': sys.stdin.read()}))
'''
            result = subprocess.run(
                [sys.executable, '-c', program, str(Path(lab.__file__).parent),
                 str(root / 'config'), mode],
                input='NEXT_SESSION_COMMAND\n', text=True, capture_output=True,
                env=dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH']),
                timeout=10, check=True)
            return json.loads(result.stdout)

    def test_read_only_ssh_preserves_controller_input(self):
        self.assertEqual(self.run_fixture('legacy'),
                         {'child': 'NEXT_SESSION_COMMAND\n', 'remaining': ''})
        self.assertEqual(self.run_fixture('readonly'),
                         {'child': '', 'remaining': 'NEXT_SESSION_COMMAND\n'})

    def test_explicit_remote_input_preserves_controller_input(self):
        self.assertEqual(self.run_fixture('explicit'),
                         {'child': 'sudo-fixture\n', 'remaining': 'NEXT_SESSION_COMMAND\n'})


if __name__ == '__main__':
    unittest.main()

"""Validate input support before opening a controller connection."""
import types
import unittest
from unittest.mock import Mock, patch

import nanokvm


class NanoKVMDependencyTests(unittest.TestCase):
    def test_missing_or_unrelated_websocket_module_fails_before_cookie_access(self):
        for module in (None, types.SimpleNamespace()):
            with patch.dict('sys.modules', {'websocket': module}), \
                    patch.object(nanokvm, 'cookie') as cookie:
                with self.assertRaisesRegex(RuntimeError, 'nanokvm/.venv/bin/python'):
                    nanokvm.send_reports([])
                cookie.assert_not_called()

    def test_available_client_can_be_checked_without_connecting(self):
        module = types.SimpleNamespace(create_connection=Mock())
        with patch.dict('sys.modules', {'websocket': module}):
            self.assertIs(nanokvm.input_client(), module)
            module.create_connection.assert_not_called()


if __name__ == '__main__':
    unittest.main()

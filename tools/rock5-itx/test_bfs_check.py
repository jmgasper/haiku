"""Reject dirty or incomplete filesystem reports even when checkfs exits zero."""
import unittest

from bfs_check import allocation_counts, require_clean


REPORT = '''       50 nodes processed\x1b[1A
        272 nodes checked,
\t0 blocks not allocated,
\t0 blocks already set,
\t0 blocks could be freed
'''


class BFSCheckTests(unittest.TestCase):
    def test_complete_clean_report(self):
        self.assertEqual(require_clean(REPORT),
                         {'missing': 0, 'duplicate': 0, 'unreferenced': 0})

    def test_nonzero_counter_cannot_match_zero_as_a_substring(self):
        for count in (8, 10, 15, 100):
            report = REPORT.replace('0 blocks could be freed',
                                    f'{count} blocks could be freed')
            self.assertEqual(allocation_counts(report)['unreferenced'], count)
            with self.assertRaisesRegex(ValueError, 'not clean'):
                require_clean(report)

    def test_missing_or_multiple_checks_are_ambiguous(self):
        for report in (REPORT.split('0 blocks could')[0], REPORT + REPORT):
            with self.assertRaisesRegex(ValueError, 'Expected one'):
                require_clean(report)

    def test_node_damage_is_not_hidden_by_zero_allocation_counters(self):
        with self.assertRaisesRegex(ValueError, 'node check failed'):
            require_clean('example (inode = 123), invalid b+tree\n' + REPORT)


if __name__ == '__main__':
    unittest.main()

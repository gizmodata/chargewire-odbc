import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('download_server', Path(__file__).with_name('download-test-server.py'))
download = importlib.util.module_from_spec(spec)
spec.loader.exec_module(download)


class CandidateTests(unittest.TestCase):
    def test_requires_successful_main_push_at_exact_commit(self):
        good = dict(head_sha='abc', head_branch='main', event='push', conclusion='success',
                    path='.github/workflows/server.yml', id=42)
        self.assertEqual(download.candidate_run([good], 'abc'), 42)
        for changed in [dict(head_sha='older'), dict(head_branch='release/test'),
                        dict(event='pull_request'), dict(conclusion='failure'),
                        dict(path='.github/workflows/other.yml')]:
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                download.candidate_run([good | changed], 'abc')

    def test_rejects_missing_or_ambiguous_checksums_and_changed_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'server.tar.gz'
            archive.write_bytes(b'candidate')
            sums = hashlib.sha256(b'candidate').hexdigest() + '  server.tar.gz\n'
            download.verify_checksum(archive, sums)
            for invalid in ['', sums + sums, sums.replace('server.tar.gz', 'other.tar.gz')]:
                with self.subTest(checksums=invalid), self.assertRaises(RuntimeError):
                    download.verify_checksum(archive, invalid)
            archive.write_bytes(b'changed')
            with self.assertRaises(RuntimeError):
                download.verify_checksum(archive, sums)


if __name__ == '__main__':
    unittest.main()

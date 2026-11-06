"""Filesystem integration tests; target power-loss/driver tests remain physical."""
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'aa_wireless_dongle/board/common/rootfs_overlay/usr/sbin/aawg-logs'


class BootLoggingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.persist = self.root / 'persist'
        self.runtime = self.root / 'runtime'
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.persist.mkdir()
        # Optional: exercise the firmware's ARM shell/applets through its loader
        # on Docker Desktop's binfmt support, instead of the host utilities.
        busybox = os.environ.get('AAWG_TEST_ARM_BUSYBOX')
        if busybox:
            loader = os.environ['AAWG_TEST_ARM_LOADER']
            libraries = os.environ['AAWG_TEST_ARM_LIBRARIES']
            for applet in ('sh', 'grep', 'wc', 'du', 'sort', 'rm', 'mkdir', 'ln',
                           'mv', 'cp', 'readlink', 'flock', 'tr', 'cut', 'sed',
                           'date', 'uname', 'cat', 'head', 'tail', 'awk', 'ip', 'tar', 'gzip'):
                arguments = [loader, '--library-path', libraries, busybox, applet]
                self.fake(applet, 'exec ' + shlex.join(arguments) + ' "$@"')
        self.config = self.root / 'config'
        self.config.write_text('AAWG_PERSISTENT_LOGS=1\n')
        self.fake('mountpoint', 'exit "${TEST_MOUNT_RESULT:-0}"')
        self.fake('df', 'printf "Filesystem 1024-blocks Used Available Capacity Mounted\\n/dev/test 32768 1000 %s 5%% /persist\\n" "${TEST_FREE_KB:-20000}"')
        self.fake('sync', 'exit 0')
        self.fake('dmesg', 'echo early-kernel-marker')
        self.env = dict(os.environ, AAWG_LOG_PERSIST=str(self.persist),
                        AAWG_LOG_RUNTIME=str(self.runtime),
                        AAWG_LOG_CONFIG=str(self.config),
                        PATH=str(self.bin) + ':' + os.environ['PATH'])

    def fake(self, name, body):
        path = self.bin / name
        path.write_text('#!/bin/sh\n' + body + '\n')
        path.chmod(0o755)

    def run_script(self, *arguments, check=True):
        return subprocess.run(['sh', str(SCRIPT), *arguments], env=self.env,
                              text=True, capture_output=True, check=check)

    def boot(self, number):
        if self.runtime.exists():
            shutil.rmtree(self.runtime)
        self.env['AAWG_LOG_BOOT_ID'] = f'{number:08x}-0000-0000-0000-000000000000'
        self.run_script('init')
        return Path(self.run_script('current').stdout.strip())

    def test_reboot_preserves_marker_and_service_restart_keeps_same_boot(self):
        first = self.boot(1)
        self.run_script('mark', 'TRUCK_ATTEMPT_1')
        self.run_script('init')
        self.assertEqual(first, Path(self.run_script('current').stdout.strip()))
        second = self.boot(2)
        self.assertNotEqual(first, second)
        self.assertIn('TRUCK_ATTEMPT_1', (first / 'stages.tsv').read_text())
        self.assertIn('persistent=yes', (second / 'boot.txt').read_text())
        self.assertIn('early-kernel-marker', (second / 'kernel-start.txt').read_text())

    def test_retains_five_boots_and_preserves_pairing_and_foreign_symlink(self):
        pairing = self.persist / 'bluetooth'
        pairing.mkdir()
        (pairing / 'identity').write_text('keep me')
        first = self.boot(1)
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'keep').write_text('keep')
        link = self.persist / 'logs/boot-000000-deadbeef'
        link.symlink_to(outside)
        for number in range(2, 9):
            self.boot(number)
        folders = [p for p in (self.persist / 'logs').glob('boot-*') if not p.is_symlink()]
        self.assertEqual(5, len(folders))
        self.assertFalse(first.exists())
        self.assertTrue((outside / 'keep').exists())
        self.assertEqual('keep me', (pairing / 'identity').read_text())

    def test_repeated_uuid_after_lost_runtime_never_overwrites(self):
        first = self.boot(1)
        self.run_script('mark', 'original')
        second = self.boot(1)
        self.assertNotEqual(first, second)
        self.assertIn('original', (first / 'stages.tsv').read_text())

    def test_missing_mount_and_low_space_fall_back_to_ram(self):
        for setting in ({'TEST_MOUNT_RESULT': '1'}, {'TEST_FREE_KB': '1'}):
            self.env.update(setting)
            directory = self.boot(1)
            self.assertTrue(directory.is_relative_to(self.runtime))
            self.assertIn('persistent=no', (directory / 'boot.txt').read_text())
            self.assertIn('persistent=no', self.run_script('list').stdout)
            self.env.pop(next(iter(setting)))

    def test_disabled_logging_uses_ram(self):
        self.config.write_text('AAWG_PERSISTENT_LOGS=0\n')
        directory = self.boot(1)
        self.assertTrue(directory.is_relative_to(self.runtime))

    def test_stage_limit_rotates_and_keeps_latest_summary(self):
        directory = self.boot(1)
        (directory / 'stages.tsv').write_text('x' * 131072)
        self.run_script('event', 'phone', 'auth', 'FAIL', 'specific failure')
        self.assertTrue((directory / 'stages.tsv.0').exists())
        self.assertLess((directory / 'stages.tsv').stat().st_size, 1024)
        self.assertIn('specific failure', (directory / 'summary.txt').read_text())

    def test_export_rejects_traversal_and_symlink(self):
        self.boot(1)
        self.assertNotEqual(0, self.run_script('export', '../bluetooth', check=False).returncode)
        link = self.persist / 'logs/boot-000000-deadbeef'
        link.symlink_to(self.root)
        self.assertNotEqual(0, self.run_script('export', link.name, check=False).returncode)

    def test_export_previous_boot_archive(self):
        first = self.boot(1)
        self.run_script('mark', 'export-marker')
        self.boot(2)
        exported = subprocess.run(['sh', str(SCRIPT), 'export', first.name], env=self.env,
                                  capture_output=True)
        self.assertEqual(0, exported.returncode, exported.stderr.decode())
        archive = exported.stdout
        import io
        import tarfile
        with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as bundle:
            stages = bundle.extractfile(first.name + '/stages.tsv').read().decode()
        self.assertIn('export-marker', stages)


if __name__ == '__main__':
    unittest.main()

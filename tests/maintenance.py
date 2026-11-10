import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / 'scripts/hypr-edgesnap-check'
assess = runpy.run_path(str(CHECKER))['assess']
ON = {'hypr-edgesnap': {'enabled': True}}


class Assess(unittest.TestCase):
    def test_disabled_failed_unloaded_and_loaded(self):
        self.assertEqual(assess({'hypr-edgesnap': {'enabled': False}}, '', '', '', set())[0], 0)
        self.assertEqual(assess({'hypr-edgesnap': {'enabled': True, 'failed': True}}, 'a', 'a', 'a', {'hypr-edgesnap'})[0], 1)
        self.assertEqual(assess(ON, 'a', 'a', 'a', set())[0], 1)
        self.assertEqual(assess(ON, 'a', 'a', 'a', {'hypr-edgesnap'})[0], 0)

    def test_deferred_and_stale_abi_are_distinct(self):
        self.assertEqual(assess(ON, 'new', 'new', 'old', set())[0], 2)
        self.assertEqual(assess(ON, 'old', 'new', 'new', set())[0], 1)

    def test_unloaded_message_names_plugin_and_fallback(self):
        message = assess(ON, 'a', 'a', 'a', set())[1]
        self.assertIn('hypr-edgesnap', message)
        self.assertIn('falls back', message)


class Wait(unittest.TestCase):
    def run_checker(self, *args, loaded_on_call):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bins = root / 'bin'
            cache = root / 'cache'
            (bins).mkdir()
            (cache / 'hypr-edgesnap').mkdir(parents=True)
            (cache / 'hypr-edgesnap/state.toml').write_text('[hypr-edgesnap]\nenabled = true\n')
            (cache / 'state.toml').write_text('[state]\nhash = "abi1"\n')
            fakes = {
                'Hyprland': 'echo \'{"abiHash": "abi1"}\'',
                'hyprctl': '''case "$*" in
  "-j version") echo '{"abiHash": "abi1"}' ;;
  "-j plugin list")
    n=$(( $(cat "$COUNTER" 2>/dev/null || echo 0) + 1 )); echo $n > "$COUNTER"
    if [ $n -ge %d ]; then echo '[{"name": "hypr-edgesnap"}]'; else echo '[]'; fi ;;
  *) echo "unexpected hyprctl $*" >&2; exit 9 ;;
esac''' % loaded_on_call,
            }
            for name, body in fakes.items():
                p = bins / name
                p.write_text('#!/bin/bash\n' + body + '\n')
                p.chmod(0o755)
            env = {**os.environ, 'PATH': f'{bins}:{os.environ["PATH"]}',
                   'HYPR_EDGESNAP_HYPRPM_CACHE': str(cache),
                   'HYPRLAND_INSTANCE_SIGNATURE': 'test', 'COUNTER': str(root / 'counter')}
            start = time.monotonic()
            p = subprocess.run([sys.executable, str(CHECKER), *args], env=env, capture_output=True, text=True, timeout=20)
            return p, time.monotonic() - start

    def test_wait_polls_until_plugin_appears(self):
        p, elapsed = self.run_checker('--wait', '3', loaded_on_call=2)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('is loaded', p.stdout)
        self.assertLess(elapsed, 3)  # stopped polling once it appeared

    def test_no_wait_reports_unloaded_immediately(self):
        p, elapsed = self.run_checker('--wait', '0', loaded_on_call=2)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn('not loaded', p.stdout)
        self.assertLess(elapsed, 1)

    def test_wait_gives_up_when_plugin_never_appears(self):
        p, _ = self.run_checker('--wait', '1', loaded_on_call=99)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)


if __name__ == '__main__':
    unittest.main()

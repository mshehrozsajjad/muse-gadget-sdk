# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EpaperAnimationTest(unittest.TestCase):
    def test_boot_does_not_query_wifi_before_connection(self):
        source = (ROOT / "main/epaper_154_status.c").read_text()
        start = source.index("static int wifi_bars_at(")
        functions = source[start:source.index("// Filled battery cells", start)]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#define WIFI_MARGIN_DB 5
#define ESP_OK 0
static bool connected;
static int queries, result;
static bool wifi_mgr_is_connected(void) { return connected; }
static int esp_wifi_sta_get_rssi(int *rssi) { queries++; *rssi = -50; return result; }
''' + functions + r'''
int main(void) {
    assert(wifi_bars(0) == 0 && queries == 0);
    connected = true;
    assert(wifi_bars(0) == 3 && queries == 1);
    result = -1;
    assert(wifi_bars(3) == 0 && queries == 2);
    connected = false;
    assert(wifi_bars(3) == 0 && queries == 2);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wifi.c"
            path.write_text(code)
            exe = str(Path(tmp) / "wifi")
            command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall",
                       "-Wextra", "-Werror", str(path), "-o", exe]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_scheduler_and_rendered_keyframes(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / "animation")
            command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra",
                       "-Werror", "-I", str(ROOT / "main"), "-I", str(ROOT / "components/muse"),
                       str(ROOT / "tests/link_epaper_animation_harness.c"),
                       str(ROOT / "avatar/muse_pixel.c"), "-lm", "-o", exe]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

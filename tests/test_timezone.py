"""Compile real settings/timezone/date code and the ESP32 HTTP sync entry point."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sender = (ROOT / "src/bsp/esp32/bsp_wifi_esp32.c").read_text()
match = re.search(r"bool bsp_time_sync_from_http_date\([^\n]+\)\n\{.*?\n\}", sender, re.S)
assert match, "HTTP time synchronization entry point not found"
compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
assert compiler, "C compiler required"
with tempfile.TemporaryDirectory(prefix="kr-timezone-") as folder:
    temporary = Path(folder)
    (temporary / "time_sync_under_test.inc").write_text(match.group())
    executable = temporary / "test_timezone"
    subprocess.run([
        compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(temporary), "-I", str(ROOT / "src/core"),
        "-I", str(ROOT / "src/bsp"), "-I", str(ROOT / "src/bsp/esp32"),
        str(ROOT / "tests/test_timezone.c"), str(ROOT / "src/core/app_settings.c"),
        str(ROOT / "src/core/app_timezone.c"), str(ROOT / "src/bsp/esp32/bsp_http_date.c"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=15)

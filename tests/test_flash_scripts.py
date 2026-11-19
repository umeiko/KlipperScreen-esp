"""Exercise release flashing without a serial device or esptool installation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


class FlashScriptTests(unittest.TestCase):
    def test_preserve_firmware_flash_settings(self):
        # Exercise both the legacy package defaults and board-specific metadata.
        for config, chip, size, offset in (
            (None, "esp32", "4MB", "0x1000"),
            ("CHIP=esp32\nFLASH_SIZE=4MB\nBL_OFFSET=0x1000\nTOUCH_CAL=1\n",
             "esp32", "4MB", "0x1000"),
            ("CHIP=esp32s3\nFLASH_SIZE=16MB\nBL_OFFSET=0x0\nTOUCH_CAL=0\n",
             "esp32s3", "16MB", "0x0"),
        ):
            with self.subTest(chip=chip, config=config), tempfile.TemporaryDirectory() as tmp:
                package = Path(tmp)
                shutil.copyfile(REPO / "tools/release/flash.sh", package / "flash.sh")
                if config is not None:
                    (package / "flash.cfg").write_text(config)
                fake = package / "esptool"
                fake.write_text(
                    "#!/usr/bin/env python3\n"
                    "import json, pathlib, sys\n"
                    "pathlib.Path('args.json').write_text(json.dumps(sys.argv[1:]))\n"
                )
                fake.chmod(0o755)
                env = dict(os.environ, PATH=str(package) + os.pathsep + os.environ["PATH"])
                result = subprocess.run(
                    ["bash", str(package / "flash.sh"), "/dev/ttyUSB-test"],
                    input="", capture_output=True, text=True, env=env, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                args = json.loads((package / "args.json").read_text())
                for option, value in (("--chip", chip), ("-p", "/dev/ttyUSB-test"),
                                      ("--flash-size", size),
                                      ("--flash-mode", "keep"), ("--flash-freq", "keep")):
                    self.assertEqual(args[args.index(option) + 1], value, args)
                self.assertEqual(args[-6:], [offset, "bootloader.bin", "0x8000",
                                            "partition-table.bin", "0x10000",
                                            "klipper_remote_display.bin"])


if __name__ == "__main__":
    unittest.main()

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / ".agents/skills/vision-framework-dev"
spec = importlib.util.spec_from_file_location("vision_skill_common", SKILL / "scripts/common.py")
common = importlib.util.module_from_spec(spec)
spec.loader.exec_module(common)

class SkillScripts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vision-skill-")
        self.base = Path(self.temp.name).resolve()
        self.framework = self.base / "framework"
        fingerprints = json.loads((SKILL / "references/compatibility.json").read_text())["interface_sha256"]
        for name in fingerprints:
            destination = self.framework / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
    def tearDown(self):
        self.temp.cleanup()
    def test_valid_interface_and_changed_interface(self):
        self.assertTrue(common.inspect(self.framework)["compatible"])
        (self.framework / "cmake/Extensions.cmake").write_text("different API")
        report = common.inspect(self.framework)
        self.assertFalse(report["compatible"])
        self.assertTrue(report["errors"])
        with self.assertRaises(ValueError):
            common.create("application", self.framework, self.base / "bad", "my-station")
        self.assertFalse((self.base / "bad").exists())
    def test_missing_interface(self):
        (self.framework / "cmake/Demo.cmake").unlink()
        self.assertFalse(common.inspect(self.framework)["compatible"])
    def test_create_separate_projects_and_refuse_overwrite(self):
        for kind in ("application", "output-plugin"):
            directory = self.base / (kind + " with spaces")
            common.create(kind, self.framework, directory, "my-project")
            marker = json.loads((directory / "vision-project.json").read_text())
            self.assertEqual(marker["kind"], kind)
            sentinel = directory / "custom.txt"
            sentinel.write_text("keep user data")
            with self.assertRaises(ValueError):
                common.create(kind, self.framework, directory, "another-project")
            self.assertEqual(sentinel.read_text(), "keep user data")
            if kind == "output-plugin":
                manifest = json.loads((directory / "manifest.json").read_text())
                self.assertEqual(manifest["plugin_id"], "user.my-project")
                self.assertEqual(manifest["entry_library"], "my-project.dll")
        self.assertTrue(common.inspect(self.framework)["compatible"])
    def test_refuse_invalid_names_and_framework_internal_destination(self):
        for name in ("../bad", "x;del", "UPPER", "vision-private", "con", "com1"):
            with self.assertRaises(ValueError):
                common.create("application", self.framework, self.base / "invalid", name)
            self.assertFalse((self.base / "invalid").exists())
        with self.assertRaises(ValueError):
            common.create("application", self.framework, self.framework / "src" / "business", "my-station")
        self.assertFalse((self.framework / "src/business").exists())
    def test_missing_parent_is_not_created(self):
        with self.assertRaises(OSError):
            common.create("application", self.framework, self.base / "missing" / "child", "my-station")
        self.assertFalse((self.base / "missing").exists())
if __name__ == "__main__":
    unittest.main()

"""Regression coverage for issue #154: self-tests cannot mask ID reuse."""
import json
from pathlib import Path
import tempfile
import unittest

from todo_cli_contract import extract_pptx_text, verify_todo_project


FAULTY_TODO = '''\
#!/usr/bin/env python3
import argparse, json
from pathlib import Path
DATA = Path(__file__).with_name("tasks.json")
def load(): return json.loads(DATA.read_text()) if DATA.exists() else []
def save(items): DATA.write_text(json.dumps(items))
def add_task(title):
    items=load(); ident=max((t["id"] for t in items), default=0)+1
    task={"id":ident,"title":title}; items.append(task); save(items); return task
def delete_task(ident):
    save([t for t in load() if t["id"]!=ident])
def main():
    p=argparse.ArgumentParser(); s=p.add_subparsers(dest="cmd", required=True)
    a=s.add_parser("add"); a.add_argument("--title", required=True)
    d=s.add_parser("delete"); d.add_argument("id", type=int)
    x=p.parse_args(); items=load()
    if x.cmd=="add":
        print(add_task(x.title)["id"])
    else:
        delete_task(x.id)
if __name__=="__main__": main()
'''

FIXED_TODO = '''\
#!/usr/bin/env python3
import argparse, json
from pathlib import Path
DATA = Path(__file__).with_name("tasks.json")
COUNTER = Path(__file__).with_name(".next-id")
def load(): return json.loads(DATA.read_text()) if DATA.exists() else []
def save(items): DATA.write_text(json.dumps(items))
def add_task(title):
    items=load(); ident=int(COUNTER.read_text()) if COUNTER.exists() else max((t["id"] for t in items), default=0)+1
    COUNTER.write_text(str(ident+1))
    task={"id":ident,"title":title}; items.append(task); save(items); return task
def delete_task(ident):
    save([t for t in load() if t["id"]!=ident])
def main():
    p=argparse.ArgumentParser(); s=p.add_subparsers(dest="cmd", required=True)
    a=s.add_parser("add"); a.add_argument("--title", required=True)
    d=s.add_parser("delete"); d.add_argument("id", type=int)
    x=p.parse_args(); items=load()
    if x.cmd=="add":
        print(add_task(x.title)["id"])
    else:
        delete_task(x.id)
if __name__=="__main__": main()
'''


class TodoCliContractTests(unittest.TestCase):
    def make_project(self, source: str, expected_last_id: int) -> Path:
        temp = tempfile.TemporaryDirectory(prefix="todo-cli-fixture-")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        (root / "todo.py").write_text(source, encoding="utf-8")
        (root / "test_todo.py").write_text(
            "import unittest\nclass T(unittest.TestCase):\n"
            " def test_id_not_reused_after_delete(self):\n"
            "  import todo\n"
            "  todo.add_task('first'); todo.add_task('second'); todo.delete_task(2)\n"
            f"  self.assertEqual(todo.add_task('third')['id'], {expected_last_id})\n"
            "if __name__ == '__main__': unittest.main()\n",
            encoding="utf-8",
        )
        return root

    def test_independent_scenario_rejects_reused_deleted_id_even_if_self_tests_pass(self):
        project = self.make_project(FAULTY_TODO, 2)
        report = verify_todo_project(project)
        self.assertTrue(report["self_tests"]["passed"])
        self.assertFalse(report["id_not_reused"]["passed"])
        self.assertEqual(report["id_not_reused"]["observed_ids"], [1, 2])
        self.assertFalse(report["passed"])

    def test_independent_scenario_accepts_monotonic_id_counter(self):
        project = self.make_project(FIXED_TODO, 3)
        report = verify_todo_project(project)
        self.assertTrue(report["self_tests"]["passed"])
        self.assertTrue(report["id_not_reused"]["passed"])
        self.assertEqual(report["id_not_reused"]["observed_ids"], [1, 3])
        self.assertTrue(report["passed"])

    def test_pptx_text_is_read_from_shapes(self):
        class Shape:
            has_text_frame = True
            text = "Regression report"

        class Slide:
            shapes = [Shape()]

        class PresentationFixture:
            slides = [Slide()]

        count, text = extract_pptx_text(PresentationFixture())
        self.assertEqual(count, 1)
        self.assertEqual(text, "Regression report")


if __name__ == "__main__":
    unittest.main()

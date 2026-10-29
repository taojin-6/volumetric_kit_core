# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
import contextlib
import io
import os
import tempfile
import unittest
from unittest import mock

from check_workflow_triggers import REFUSED, check, main, triggers

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "testdata", "triggers")
ROOT = os.path.dirname(os.path.dirname(HERE))
JOBS = "jobs:\n  build:\n    runs-on: ubuntu-latest\n    steps:\n      - run: echo build\n"


def forms(event):
    """A workflow triggered by push and EVENT in each of YAML's forms, with
    the line EVENT is on."""
    return {
        "a name": (f"on: {event}\n", 1),
        "a quoted name": (f"on: '{event}'\n", 1),
        "a flow list": (f"on: [push, {event}]\n", 1),
        "a flow list over lines": (f"on: [\n  push,\n  \"{event}\" # why\n]\n", 3),
        "a block list": (f"on:\n  - push\n  - {event}\n", 3),
        "a block list as deep as its key": (f"on:\n- push\n- \"{event}\"\n", 3),
        "a block map": (f"on:\n  push:\n    branches: [main]\n  {event}:\n    types: [a]\n", 4),
        "a block map, quoted": (f"\"on\":\n  push:\n  '{event}': {{}}\n", 3),
        "a flow map": (f"on: {{push: {{branches: [main]}}, {event}: {{types: [a]}}}}\n", 1),
        "a flow map over lines": (f"'on': {{\n  push: {{}},\n  \"{event}\":{{}}\n}}\n", 3),
    }


def run(directory, env=None):
    """main() on DIRECTORY: its exit code and what it printed."""
    out = io.StringIO()
    with mock.patch.dict(os.environ, env or {"GITHUB_ACTIONS": ""}), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = main([directory])
    return code, out.getvalue()


class TriggerTest(unittest.TestCase):
    def check_text(self, text):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "ci.yml"), "w", encoding="utf-8") as file:
                file.write(text + JOBS)
            refusals, count = check(directory)
        self.assertEqual(count, 1)
        return [(line, message) for _, line, message in refusals]

    def test_every_refused_event_in_every_form_is_named_with_its_line(self):
        for event in REFUSED:
            for form, (text, line) in forms(event).items():
                with self.subTest(event=event, form=form):
                    self.assertIn((event, line), triggers(text))
                    self.assertEqual(self.check_text(text),
                                     [(line, f"on: {event} is refused: {REFUSED[event]}")])

    def test_allowed_events_in_every_form_pass(self):
        for event in ("pull_request", "workflow_dispatch", "workflow_call", "merge_group"):
            for form, (text, line) in forms(event).items():
                with self.subTest(event=event, form=form):
                    self.assertIn((event, line), triggers(text))
                    self.assertEqual(self.check_text(text), [])

    def test_a_refused_name_in_any_case_is_refused(self):
        self.assertEqual([line for line, _ in self.check_text("on: [Schedule]\n")], [1])

    def test_unreadable_triggers_fail(self):
        for why, text in {
            "no on": "name: x\n",
            "an empty on": "on:\nname: x\n",
            "an anchor": "on: &t\n  push:\n",
            "an alias": "on: *t\n",
            "a tag": "on: !!str push\n",
            "a bracket never closed": "on: [push,\n  pull_request\n",
            "text after the bracket": "on: [push] x\n",
            "a list and a map": "on:\n  - push\n  pull_request:\n",
            "a map entry with no key": "on:\n  push\n",
            "an escape in a quoted name": 'on: ["sched\\u0075le"]\n',
            "a quote never closed": "on:\n  'push:\n",
        }.items():
            with self.subTest(why=why):
                refusals = self.check_text(text)
                self.assertEqual(len(refusals), 1, refusals)
                self.assertTrue(refusals[0][1].startswith("cannot read its triggers: "), refusals)

    def test_the_allowed_fixtures_pass(self):
        refusals, count = check(os.path.join(FIXTURES, "allowed"))
        self.assertEqual((refusals, count), ([], 5))

    def test_the_refused_fixtures_name_each_file_line_and_event(self):
        refusals, count = check(os.path.join(FIXTURES, "refused"))
        found = [(os.path.basename(path), line, message.split(" is refused")[0])
                 for path, line, message in refusals]
        self.assertEqual(count, 7)
        self.assertEqual(found, [
            ("block_list.yml", 5, "on: pull_request_review_comment"),
            ("block_map.yml", 5, "on: schedule"),
            ("block_map.yml", 7, "on: pull_request_target"),
            ("flow_map.yaml", 4, "on: discussion_comment"),
            ("flow_map.yaml", 5, "on: workflow_run"),
            ("list.yml", 2, "on: discussion"),
            ("list_at_key.yml", 4, "on: workflow_run"),
            ("name.yml", 2, "on: issue_comment"),
            ("unreadable.yml", 3, "cannot read its triggers: cannot read the name '&triggers'"),
        ])

    def test_main_exits_nonzero_naming_file_and_line(self):
        code, out = run(os.path.join(FIXTURES, "allowed"))
        self.assertEqual(code, 0, out)
        code, out = run(os.path.join(FIXTURES, "refused"))
        self.assertEqual(code, 1)
        path = os.path.relpath(os.path.join(FIXTURES, "refused", "block_map.yml"))
        self.assertIn(f"{path}:5: on: schedule is refused: ", out)
        self.assertIn("9 refused in 7 workflows", out)

    def test_main_annotates_in_github_actions(self):
        code, out = run(os.path.join(FIXTURES, "refused"), {"GITHUB_ACTIONS": "true"})
        path = os.path.relpath(os.path.join(FIXTURES, "refused", "name.yml"))
        self.assertEqual(code, 1)
        self.assertIn(f"::error file={path},line=2::on: issue_comment is refused: ", out)

    def test_main_fails_with_no_workflow_to_read(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(run(directory)[0], 1)
            self.assertEqual(run(os.path.join(directory, "missing"))[0], 1)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main([]), 2)

    def test_the_action_runs_this_script(self):
        action = os.path.join(ROOT, ".github", "actions", "check-workflow-triggers")
        with open(os.path.join(action, "action.yml"), encoding="utf-8") as file:
            text = file.read()
        script = "$GITHUB_ACTION_PATH/../../../tools/github/check_workflow_triggers.py"
        self.assertIn(script, text)
        self.assertTrue(os.path.samefile(
            os.path.join(action, "../../../tools/github/check_workflow_triggers.py"),
            os.path.join(HERE, "check_workflow_triggers.py")))


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
import copy
import unittest

from required_check import ApiError, audit


class Server:
    def __init__(self, protection=None):
        self.protection = protection
        self.calls = []
        self.checks = [{"name": "required", "app": {"id": 15368}}]
        self.pages = None
        self.fail_write = False
        self.fail_read = False
        self.stale = False
        self.head_changed = False
        self.ignore_write = False
        self.branch_reads = 0

    def __call__(self, method, endpoint, payload=None):
        self.calls.append((method, endpoint, copy.deepcopy(payload)))
        if method == "GET":
            if endpoint.endswith("/branches/main"):
                self.branch_reads += 1
                if self.stale and self.branch_reads == 2:
                    self.protection = {"enforce_admins": {"enabled": True}}
                return {"protected": self.protection is not None,
                        "commit": {"sha": "def" if self.head_changed and self.branch_reads > 1 else "abc"}}
            if endpoint.endswith("/protection"):
                if self.fail_read:
                    raise ApiError("Forbidden")
                return copy.deepcopy(self.protection)
            if "/check-runs?" in endpoint:
                if self.pages is not None:
                    page = int(endpoint.rsplit("=", 1)[1])
                    return {"check_runs": self.pages[page - 1]}
                return {"check_runs": self.checks}
            raise AssertionError(endpoint)
        if self.fail_write:
            raise ApiError("Forbidden")
        if self.ignore_write:
            return {}
        if method == "PUT":
            self.protection = copy.deepcopy(payload)
            self.protection["enforce_admins"] = {"enabled": payload["enforce_admins"]}
        elif method == "PATCH":
            self.protection["required_status_checks"] = copy.deepcopy(payload)
        elif method == "POST":
            self.protection["enforce_admins"] = {"enabled": True}
        else:
            raise AssertionError(method)
        return copy.deepcopy(self.protection)


class PolicyTest(unittest.TestCase):
    def run_policy(self, server, apply=False):
        return audit(server, "owner/project", "main", "required", 15368, apply)

    def test_default_only_reads_and_reports_missing_protection(self):
        server = Server()
        report = self.run_policy(server)
        self.assertFalse(report["compliant_before"])
        self.assertFalse(report["applied"])
        self.assertTrue(all(call[0] == "GET" for call in server.calls))

    def test_create_verify_and_idempotent(self):
        server = Server()
        report = self.run_policy(server, True)
        self.assertTrue(report["applied"])
        self.assertTrue(report["after"]["required_status_checks"]["strict"])
        server.calls.clear()
        self.assertTrue(self.run_policy(server, True)["compliant_before"])
        self.assertTrue(all(call[0] == "GET" for call in server.calls))

    def test_existing_policy_and_other_checks_are_preserved(self):
        reviews = {"required_approving_review_count": 2}
        server = Server({"required_status_checks": {
            "strict": False, "contexts": ["legacy", "lint", "required"],
            "checks": [{"context": "lint", "app_id": 42},
                       {"context": "required", "app_id": -1}]},
            "enforce_admins": {"enabled": False},
            "required_pull_request_reviews": reviews,
            "restrictions": {"users": [{"login": "owner"}]},
            "required_signatures": {"enabled": True}})
        before = copy.deepcopy(server.protection)
        self.run_policy(server, True)
        for key in ("required_pull_request_reviews", "restrictions", "required_signatures"):
            self.assertEqual(server.protection[key], before[key])
        self.assertEqual(server.protection["required_status_checks"]["checks"], [
            {"context": "lint", "app_id": 42}, {"context": "legacy", "app_id": -1},
            {"context": "required", "app_id": 15368}])
        self.assertNotIn("PUT", [call[0] for call in server.calls])

    def test_unobserved_check_or_wrong_app_never_writes(self):
        for checks in ([], [{"name": "required", "app": {"id": 7}}]):
            server = Server()
            server.checks = checks
            with self.assertRaisesRegex(ApiError, "have not reported"):
                self.run_policy(server, True)
            self.assertTrue(all(call[0] == "GET" for call in server.calls))

    def test_observation_reads_later_pages(self):
        server = Server()
        server.pages = [
            [{"name": "another-check", "app": {"id": 15368}}] * 100,
            server.checks,
        ]
        self.assertTrue(self.run_policy(server, True)["applied"])
        self.assertTrue(any("page=2" in call[1] for call in server.calls))

    def test_permissions_failure_is_not_unprotected(self):
        server = Server({})
        server.fail_read = True
        with self.assertRaisesRegex(ApiError, "Forbidden"):
            self.run_policy(server, True)
        self.assertTrue(all(call[0] == "GET" for call in server.calls))

    def test_write_failure_is_not_success(self):
        server = Server()
        server.fail_write = True
        with self.assertRaisesRegex(ApiError, "Forbidden"):
            self.run_policy(server, True)
        self.assertIsNone(server.protection)

    def test_concurrent_edit_is_not_overwritten(self):
        server = Server()
        server.stale = True
        with self.assertRaisesRegex(ApiError, "changed during"):
            self.run_policy(server, True)
        self.assertTrue(all(call[0] == "GET" for call in server.calls))

    def test_advanced_head_is_checked_again_on_next_run(self):
        server = Server()
        server.head_changed = True
        with self.assertRaisesRegex(ApiError, "changed during"):
            self.run_policy(server, True)
        self.assertTrue(all(call[0] == "GET" for call in server.calls))

    def test_missing_status_section_is_not_replaced_with_blank_rules(self):
        server = Server({"required_pull_request_reviews": {"required_approving_review_count": 2}})
        with self.assertRaisesRegex(ApiError, "no status checks"):
            self.run_policy(server, True)
        self.assertTrue(all(call[0] == "GET" for call in server.calls))

    def test_apply_must_verify_observed_policy(self):
        server = Server({"required_status_checks": {"strict": False, "contexts": []}})
        server.ignore_write = True
        with self.assertRaisesRegex(ApiError, "did not verify"):
            self.run_policy(server, True)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Audit, or explicitly apply, the required CI gate on one repository branch."""

import argparse
import json
import subprocess
import sys
from urllib.parse import quote


class ApiError(RuntimeError):
    pass


def gh(method, endpoint, payload=None):
    command = ["gh", "api", "--method", method, endpoint,
               "-H", "Accept: application/vnd.github+json"]
    if payload is not None:
        command += ["--input", "-"]
    result = subprocess.run(command, input=json.dumps(payload) if payload is not None else None,
                            capture_output=True, text=True, check=False)
    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ApiError(f"{method} {endpoint}: invalid API response") from error
    if result.returncode:
        raise ApiError(f"{method} {endpoint}: {data.get('message', 'request failed')}")
    return data


def checks_of(protection):
    required = protection.get("required_status_checks") or {}
    checks = list(required.get("checks") or [])
    known = {check["context"] for check in checks}
    checks += [{"context": context, "app_id": -1}
               for context in required.get("contexts", []) if context not in known]
    return [{"context": check["context"], "app_id": check.get("app_id") or -1}
            for check in checks]


def satisfies(protection, context, app_id):
    return (bool((protection.get("required_status_checks") or {}).get("strict"))
            and bool((protection.get("enforce_admins") or {}).get("enabled"))
            and {"context": context, "app_id": app_id} in checks_of(protection))


def audit(api, repo, branch, context, app_id, apply=False):
    """Preserve existing protections; change only checks and admin enforcement.

    Read the branch first: a 404 from a protection endpoint is ambiguous (it
    can mean insufficient access). Only a branch explicitly reporting itself
    unprotected may receive a new protection document.
    """
    root = f"repos/{repo}/branches/{quote(branch, safe='')}"
    branch_info = api("GET", root)
    protected = branch_info["protected"]
    before = api("GET", root + "/protection") if protected else {}
    compliant = satisfies(before, context, app_id)
    checks = [check for check in checks_of(before) if check["context"] != context]
    checks.append({"context": context, "app_id": app_id})
    status_checks = {"strict": True, "checks": checks}
    report = {"repository": repo, "branch": branch, "compliant_before": compliant,
              "before": before, "desired_status_checks": status_checks,
              "desired_enforce_admins": True, "applied": False}
    if not apply or compliant:
        return report

    # Refuse a typo or an unproven app binding before changing policy. All pages
    # are inspected because repositories may have more than 100 check runs.
    ref = branch_info["commit"]["sha"]
    page = 1
    observed = False
    while True:
        runs = api("GET", f"repos/{repo}/commits/{ref}/check-runs?per_page=100&page={page}")
        current = runs["check_runs"]
        observed |= any(run["name"] == context and run["app"]["id"] == app_id
                        for run in current)
        if observed or len(current) < 100:
            break
        page += 1
    if not observed:
        raise ApiError("The requested check and app have not reported on the branch head")

    # A policy may change while the audit is running. Refuse rather than
    # overwrite somebody else's edit; this is a best-effort race check, not a
    # server-side compare-and-swap (the API offers no atomic policy update).
    latest_branch = api("GET", root)
    latest = api("GET", root + "/protection") if latest_branch["protected"] else {}
    if (latest_branch["protected"] != protected or latest != before
            or latest_branch["commit"]["sha"] != ref):
        raise ApiError("Branch or protection changed during the audit; run again")
    if protected:
        if not before.get("required_status_checks"):
            raise ApiError("Existing protection has no status checks; enable them before applying")
        api("PATCH", root + "/protection/required_status_checks", status_checks)
        api("POST", root + "/protection/enforce_admins")
    else:
        api("PUT", root + "/protection", {
            "required_status_checks": status_checks, "enforce_admins": True,
            "required_pull_request_reviews": {"required_approving_review_count": 0},
            "restrictions": None})
    after = api("GET", root + "/protection")
    report["after"] = after
    report["applied"] = satisfies(after, context, app_id)
    if not report["applied"]:
        raise ApiError("Policy update did not verify; inspect branch protection")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", help="owner/repository")
    parser.add_argument("--branch", default="main")
    parser.add_argument("--context", default="required")
    parser.add_argument("--app-id", type=int, default=15368, help="GitHub Actions app ID")
    parser.add_argument("--apply", action="store_true", help="write and verify the policy")
    args = parser.parse_args()
    if len(args.repository.split("/")) != 2 or any(not part for part in args.repository.split("/")):
        parser.error("repository must be owner/repository")
    if args.app_id <= 0:
        parser.error("app-id must identify a specific GitHub App")
    try:
        report = audit(gh, args.repository, args.branch, args.context, args.app_id, args.apply)
    except (ApiError, KeyError, TypeError, OSError) as error:
        print(f"required-check policy: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["compliant_before"] or report["applied"] else 1


if __name__ == "__main__":
    sys.exit(main())

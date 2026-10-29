# GitHub tools

Checks of a repository's GitHub settings and workflows, in Python's standard
library and the GitHub CLI. Run their host-only tests with:

```sh
python3 -m unittest discover -s tools/github -p 'test_*.py' -v
```

## Workflow triggers

`check_workflow_triggers.py <directory>` reads every `*.yml` and `*.yaml`
workflow in the directory and fails, naming the file and line, on a trigger
an outsider can cause or a schedule
([tools/runners/README.md](../runners/README.md#who-can-trigger-ci) says
which and why). It reads `on:` as a name, a list or a map, in block or flow
style, and fails on one it cannot read, such as an anchor. Exit 0 means no
refused trigger, 1 a refused or unreadable one, or no workflow to read.
`.github/actions/check-workflow-triggers` runs it on the repository checked
out; in GitHub Actions each refusal is an error annotation on its line.

## Required CI check policy

The `required` job aggregates CI results. A green job is a merge gate only
when branch protection requires it, including for repository administrators.
This tool audits one explicit repository and branch using the authenticated
GitHub CLI. It does not infer a repository list or run automatically.

```sh
python3 tools/github/required_check.py owner/repository
python3 tools/github/required_check.py owner/repository --apply
```

The default is read-only. It prints the current protection and desired status
checks as JSON; exit 0 means compliant, 1 means an update is needed, and 2
means the audit or update failed. Save the output in the operator's audit log.
`--apply` requires repository administration permission. It first verifies
that `required` has reported from the GitHub Actions app on the branch head,
then requires that check with up-to-date branches and administrator
enforcement. Use `--branch`, `--context`, and `--app-id` for another gate.

Existing review rules, push restrictions, signatures and unrelated required
checks remain in place. An unprotected branch receives a new policy requiring
a pull request, status checks and administrator enforcement, with no
reviewer-approval requirement or push allowlist. An existing policy with no
status-check section is refused: enable that section manually first. Existing protection uses the narrow status-check and
administrator endpoints. A changed branch head or policy during the audit is refused;
GitHub offers no atomic compare-and-swap for these settings. A failure between
updates can leave a partial policy: inspect the response and rerun the audit.
The final policy is read back and verified. A source PR alone does not apply
GitHub repository settings.

This checks classic branch protection, not rulesets, workflow permissions or
runner security. A protected branch whose protection is unreadable is an
error, never treated as an empty policy. Private-repository protection depends
on the account's GitHub plan.

API behavior: [GitHub branch protection documentation](https://docs.github.com/en/rest/branches/branch-protection).

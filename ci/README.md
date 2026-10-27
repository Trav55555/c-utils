# Private CI settings

The public GitHub workflow runs offline CI-helper tests and native C tests.
It needs no private infrastructure or credentials.

The optional Gitea workflow uses private repository variables for its deployment
settings. It accepts pushes and manual runs only from the configured repository
and actor, and scheduled runs only in the configured repository. Gitea does not
attribute a scheduled run to a pusher, so its actor is not checked; a schedule
runs only the workflow committed on the default branch, which already takes
repository write access to change. Missing authorization variables do not
authorize a job. There is no default runner, API login, or source repository.

Set these variables in the private repository before publishing changes to this
workflow:

| Variable | Value |
|---|---|
| `CUTILS_CI_REPOSITORY` | Exact trusted repository, in `owner/repository` form |
| `CUTILS_CI_ACTOR` | Exact trusted push and manual-run actor |
| `CUTILS_CI_RUNNER` | Label of the repository-scoped Linux x86-64 host runner |
| `CUTILS_CI_API_LOGIN` | Existing `tea` login profile on that runner |
| `CUTILS_CI_TOOLCHAIN_REPOSITORY` | Private toolchain repository, in `owner/repository` form |

Keep API credentials in the runner's existing login configuration. Do not put
tokens or passwords in repository variables, source files, or test fixtures.
The login profile selects the server without publishing its address.

`dependencies.json` pins the dependency commit and complete Git tree. Private
settings select its location, not its expected contents. The fetcher validates
its settings before invoking the API client, rejects unsafe archives, and
checks the extracted tree. It never falls back to another login or transport.

A push to `main` or a `verify/**` branch runs the gate tier, `dcheck gate`,
and passes only if regions is among its passes. The deep tier (the gate's
checks plus Cerberus, Valgrind, fuzzing, and CBMC) runs nightly at 06:00 UTC
and on demand from the Actions tab (`workflow_dispatch`). Each event has its
own concurrency group, so a push does not cancel a nightly run, and a lock on
the runner runs them one at a time.

The nightly run calls `make deep` with `DCHECK_JSON`, so dcheck writes its
JSON result (`result.json`) instead of its summary. `ci/findings.py` judges
the run from it: the run passes only if dcheck passes with regions among its
passes. It prints each check's status to the job log and keeps one open issue
titled "Nightly findings", through the `CUTILS_CI_API_LOGIN` profile in the
trusted repository. A failing run opens that issue, or replaces its
description with the current findings and comments; a passing run comments
and closes it. A finding is a failed check with its diagnostics and the last
60 lines of its log. Work from these findings rather than running `make deep`
locally. A failed issue update is logged and does not change the verdict.
Only a call Gitea did not answer is retried, for up to 20 minutes in all; a
create whose answer was lost is looked up before it is sent again. A run that
stops before `make deep` starts (a pin mismatch, a toolchain that does not
build) still opens the findings issue. The lock is held until the issue is
kept, so two runs cannot both open one.

Both tiers retain the checker profile validation, regions build, 3 GiB memory
limit, and disabled swap, and keep their logs, plus `result.json` for the
nightly run, as the run's artifact for 30 days. The deep tier keeps its
2,400-second aggregate timeout and complete deep-check workload; the gate has
900 seconds. Both call the same `tools/verify_deep.py` scope runner as
`make deep`. The scope kills all its processes at the deadline, including
new process groups and SIGTERM-ignoring children. The runner also rejects
leftover children after a successful checker exit. Public workflow tests report
live cases as skipped if the user systemd manager or required Linux APIs are
unavailable. Trusted private CI sets `VERIFY_REQUIRE_SCOPE=1`, so missing live
scope prerequisites fail that job instead. Offline cleanup/error tests always run.
Variable configuration does not replace repository access controls or runner
isolation. Run an authorized CI push after configuration to verify routing and
execution. Offline tests check the workflow's guard structure but do not prove
server-side authorization.

Changing these files does not remove old values from Git history, forks,
cached pages, CI logs, or existing clones. History rewriting and credential
rotation are separate operations. Do not treat anonymized configuration as a
substitute for authentication, patching, or least-privilege credentials.

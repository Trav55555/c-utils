# Private CI settings

The public GitHub workflow runs offline CI-helper tests and native C tests.
It needs no private infrastructure or credentials.

The optional Gitea workflow uses private repository variables for its deployment
settings. It accepts pushes only from the configured repository and actor.
Missing authorization variables do not authorize a job. There is no default
runner, API login, or source repository.

Set these variables in the private repository before publishing changes to this
workflow:

| Variable | Value |
|---|---|
| `CUTILS_CI_REPOSITORY` | Exact trusted repository, in `owner/repository` form |
| `CUTILS_CI_ACTOR` | Exact trusted push actor |
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

The deep workflow retains its checker profile validation, regions build,
3 GiB memory limit, disabled swap, timeouts, and complete deep-check workload.
Variable configuration does not replace repository access controls or runner
isolation. Run an authorized CI push after configuration to verify routing and
execution. Offline tests check the workflow's guard structure but do not prove
server-side authorization.

Changing these files does not remove old values from Git history, forks,
cached pages, CI logs, or existing clones. History rewriting and credential
rotation are separate operations. Do not treat anonymized configuration as a
substitute for authentication, patching, or least-privilege credentials.

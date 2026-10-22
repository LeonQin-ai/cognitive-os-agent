# Half-hourly issue review

`issue-autofix.yml` runs on the default branch every 30 minutes and can also be
started with **Run workflow**. GitHub may delay scheduled jobs. Each run reads
the open issues and newest comments, reproduces one actionable problem, and
opens a repair PR only after the model's targeted checks and independent Linux
checks pass. It does not merge, close issues, or claim unavailable real-provider
or Windows evidence. Issues with an existing automated repair PR are skipped.

To activate actual repairs, configure the repository Actions secret
`OPENAI_API_KEY`, using a key with access and budget for Codex. The workflow never
prints it. Without that secret, issue inspection still runs and records the
findings in the run summary; automated repair reports inactive and makes no
model request. Enable the repository setting allowing GitHub Actions to
create pull requests if it is disabled. These repository settings cannot be
verified or populated through this chat's Git-only GitHub authentication.

The model runs with the workspace permission profile and no repository token in
its environment. A trusted publication step checks the patch scope, publishes a
separate branch, and explicitly dispatches `ci.yml`: pushes and PRs created with
`GITHUB_TOKEN` do not automatically trigger push/PR workflows. The repair step is
limited to 15 minutes, the whole job to 25, with no overlapping repair jobs.

Review the Actions run summary for actual findings, checks, limitations and the
PR link. A green workflow that reports missing model access means no repair ran;
it is not evidence that the application issue is fixed. For production failures,
include the running version, protocol setting, task JSON and redacted model
diagnostic in the issue so a later run can reproduce the same behavior.

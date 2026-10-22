const fs = require('node:fs');
const {execFileSync} = require('node:child_process');
const assert = require('node:assert/strict');
const directory = 'backend/build/issue-autofix';
const git = (...args) => execFileSync('git', args, {encoding: 'utf8'});
const marker = number => `<!-- issue-autofix:${number} -->`;

async function collect({github, context, core}) {
  const repo = context.repo;
  const prs = await github.paginate(github.rest.pulls.list, {...repo, state: 'open', per_page: 100});
  const open = await github.paginate(github.rest.issues.listForRepo,
    {...repo, state: 'open', sort: 'updated', direction: 'desc', per_page: 100});
  const issues = [];
  for (const issue of open.filter(issue => !issue.pull_request &&
      !prs.some(pr => (pr.body || '').includes(marker(issue.number)))).slice(0, 10)) {
    const page = Math.max(1, Math.ceil(issue.comments / 10));
    const comments = issue.comments ? (await github.rest.issues.listComments(
      {...repo, issue_number: issue.number, per_page: 10, page})).data : [];
    issues.push({number: issue.number, title: issue.title, url: issue.html_url,
      body: (issue.body || '').slice(0, 12000), updated_at: issue.updated_at,
      comments: comments.map(comment => ({author: comment.user?.login,
        created_at: comment.created_at, body: (comment.body || '').slice(0, 6000)}))});
  }
  fs.mkdirSync(directory, {recursive: true});
  fs.writeFileSync(`${directory}/context.json`, JSON.stringify({issues}, null, 2));
  core.setOutput('actionable', String(issues.length > 0));
  core.info(`Collected ${issues.length} open issues without a pending automated repair PR.`);
  await core.summary.addHeading('Open issues checked')
    .addList(issues.map(issue => `#${issue.number}: ${issue.title} (${issue.url || ''})`)).write();
}

function readResult() {
  const result = JSON.parse(fs.readFileSync(`${directory}/result.json`, 'utf8'));
  assert(['fixed', 'blocked', 'no_action'].includes(result.status), 'Invalid result status');
  assert(Number.isSafeInteger(result.issue_number) && result.issue_number >= 0, 'Invalid issue number');
  for (const name of ['title', 'summary']) assert.equal(typeof result[name], 'string');
  for (const name of ['tests', 'limitations'])
    assert(Array.isArray(result[name]) && result[name].every(value => typeof value === 'string'));
  if (result.status === 'fixed') {
    const {issues} = JSON.parse(fs.readFileSync(`${directory}/context.json`, 'utf8'));
    assert(issues.some(issue => issue.number === result.issue_number), 'Issue was not inspected');
    assert(result.title.trim() && result.summary.trim() && result.tests.length, 'Missing repair evidence');
  }
  return result;
}

async function report({core}) {
  const result = readResult();
  core.setOutput('fixed', String(result.status === 'fixed'));
  await core.summary.addHeading(`Issue review: ${result.status}`)
    .addRaw(result.summary).addBreak().addList(result.tests)
    .addHeading('Remaining limitations', 3).addList(result.limitations).write();
}

function validatePaths(paths) {
  assert(paths.length, 'No patch to publish');
  for (const path of paths) {
    assert((path.startsWith('backend/') || ['README.md', 'README.zh-CN.md'].includes(path)) &&
      !path.split('/').includes('..') && !path.split('/').includes('AGENTS.md'),
    `Automated repair cannot modify ${path}`);
    if (fs.existsSync(path)) {
      const stat = fs.lstatSync(path);
      assert(stat.isFile() && stat.size <= 1024 * 1024, `Unsupported artifact: ${path}`);
      assert(!fs.readFileSync(path).includes(0), `Binary artifact: ${path}`);
    }
  }
}

async function publish({github, context, core}) {
  const result = readResult();
  assert.equal(result.status, 'fixed');
  const repo = context.repo;
  const issue = (await github.rest.issues.get({...repo, issue_number: result.issue_number})).data;
  if (issue.state !== 'open') return core.info('Issue was closed; no PR published.');
  const prs = await github.paginate(github.rest.pulls.list, {...repo, state: 'open', per_page: 100});
  if (prs.some(pr => (pr.body || '').includes(marker(result.issue_number))))
    return core.info('A repair PR is already pending; no duplicate published.');
  const paths = [...new Set((git('diff', '--name-only', '-z', 'HEAD') +
    git('ls-files', '--others', '--exclude-standard', '-z')).split('\0').filter(Boolean))];
  validatePaths(paths);
  const branch = `codex/issue-${result.issue_number}-${context.runId}`;
  git('checkout', '-b', branch);
  git('add', '--', ...paths);
  git('diff', '--cached', '--check');
  git('-c', 'user.name=github-actions[bot]', '-c', 'user.email=41898282+github-actions[bot]@users.noreply.github.com',
    'commit', '-m', `fix: issue #${result.issue_number} ${result.title.replace(/[\r\n]/g, ' ').slice(0, 100)}`);
  execFileSync('gh', ['auth', 'setup-git'], {stdio: 'inherit'});
  git('push', 'origin', `HEAD:refs/heads/${branch}`);
  const body = [marker(result.issue_number), `Refs #${result.issue_number}`, result.summary,
    'Validation:', ...result.tests.map(test => `- ${test}`),
    'Remaining limitations:', ...result.limitations.map(item => `- ${item}`),
    'Independent Linux unit/scenario/benchmark and completion regressions passed before publication. Full CI is dispatched separately. This PR does not close the issue or merge itself.'].join('\n\n');
  const pr = (await github.rest.pulls.create({...repo, base: context.payload.repository.default_branch,
    head: branch, title: `Issue #${result.issue_number}: ${result.title.slice(0, 150)}`, body})).data;
  core.info(`Published reviewable repair: ${pr.html_url}`);
  // GITHUB_TOKEN pushes/PR creation do not trigger push/pull_request workflows.
  await github.rest.actions.createWorkflowDispatch({...repo, workflow_id: 'ci.yml', ref: branch});
  await core.summary.addLink('Repair PR', pr.html_url).write();
}

module.exports = {collect, report, publish, readResult, validatePaths};

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const worker = require('./issue-autofix.cjs');

test('scheduled repair validates evidence, scope and duplicate handling without remote writes', async t => {
  const previous = process.cwd();
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'coa-issue-worker-'));
  process.chdir(directory);
  try {
    const output = {};
    const summary = {addHeading() {return this;}, addRaw() {return this;},
      addBreak() {return this;}, addList() {return this;}, async write() {}};
    const core = {setOutput(name, value) {output[name] = value;}, info() {}, summary};
    const pulls = Array.from({length: 10}, (_, i) => ({body: `<!-- issue-autofix:${i+1} -->`}));
    const issues = Array.from({length: 11}, (_, i) => ({number: i+1,
      title: `Issue ${i+1}`, body: 'reproduction', comments: i === 10 ? 23 : 0}));
    const listPRs = () => {};
    const listIssues = () => {};
    const comments = [];
    const github = {rest: {pulls: {list: listPRs}, issues: {listForRepo: listIssues,
      async listComments(args) {comments.push(args); return {data: [{body: 'latest evidence'}]};},
      async get() {return {data: {state: 'open'}};}}},
      async paginate(method) {return method === listPRs ? pulls : issues;}};
    const context = {repo: {owner: 'fixture', repo: 'fixture'}};
    await t.test('pending PRs do not starve the next actionable issue', async () => {
      await worker.collect({github, context, core});
      const data = JSON.parse(fs.readFileSync('backend/build/issue-autofix/context.json'));
      assert.deepEqual(data.issues.map(issue => issue.number), [11]);
      assert.equal(data.issues[0].comments[0].body, 'latest evidence');
      assert.equal(comments[0].page, 3);
      assert.equal(output.actionable, 'true');
    });
    const result = {status: 'fixed', issue_number: 11, title: 'repair',
      summary: 'reproduced and repaired', tests: ['targeted regression passed'], limitations: []};
    const save = value => fs.writeFileSync('backend/build/issue-autofix/result.json', JSON.stringify(value));
    await t.test('fixed reports require inspected issues and actual test evidence', () => {
      save({...result, issue_number: 12}); assert.throws(() => worker.readResult(), /not inspected/);
      save({...result, tests: []}); assert.throws(() => worker.readResult(), /Missing repair evidence/);
      save(result); assert.deepEqual(worker.readResult(), result);
    });
    await t.test('blocked reports remain blocked and never request publication', async () => {
      save({...result, status: 'blocked', tests: [], limitations: ['Windows runner unavailable']});
      await worker.report({core}); assert.equal(output.fixed, 'false');
      save(result); await worker.report({core}); assert.equal(output.fixed, 'true');
    });
    await t.test('automation cannot publish workflow edits, instruction edits or binary/symlink artifacts', () => {
      fs.writeFileSync('backend/source.c', 'int repaired;');
      worker.validatePaths(['backend/source.c']);
      for (const name of ['.github/workflows/ci.yml', 'AGENTS.md', 'backend/AGENTS.md', 'unrelated.txt'])
        assert.throws(() => worker.validatePaths([name]), /cannot modify/);
      fs.writeFileSync('backend/binary', Buffer.from([0, 1]));
      assert.throws(() => worker.validatePaths(['backend/binary']), /Binary artifact/);
      fs.symlinkSync('source.c', 'backend/link');
      assert.throws(() => worker.validatePaths(['backend/link']), /Unsupported artifact/);
    });
    await t.test('an existing repair PR prevents all Git and PR mutations', async () => {
      pulls.push({body: '<!-- issue-autofix:11 -->'});
      // No Git repository exists in this temporary directory. Any accidental
      // mutation would fail this test rather than touching a real checkout.
      await worker.publish({github, context, core});
    });
  } finally {
    process.chdir(previous);
    fs.rmSync(directory, {recursive: true, force: true});
  }
});

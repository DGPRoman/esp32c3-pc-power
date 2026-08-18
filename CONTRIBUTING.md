# Contributing

## Commit messages

Subjects follow [Conventional Commits](https://www.conventionalcommits.org):

```
type(optional-scope)!: description
```

Bodies do not change. This repository has always explained *why* a change is
right rather than restating the diff, and that is the part worth keeping — the
subject line simply gains a prefix that a tool can read.

| Part | Rule |
| --- | --- |
| `type` | one of `build` `chore` `ci` `docs` `feat` `fix` `perf` `refactor` `revert` `style` `test` |
| `scope` | optional, lower case, named after the component: `http_server` `device_auth` `wifi_manager` `wifi_store` `hw_gpio` `hw_i2c` `hw_reg` `ssd1306` `font5x7` `power` `main` `docs` `ci` |
| `!` | append to the type or scope for a breaking change, and explain it in a `BREAKING CHANGE:` footer |
| `description` | lower case, imperative, no trailing full stop, whole subject within 72 characters |
| body | separated by one blank line, wrapped at 72, present for anything not self-evident |

A breaking change here usually means the HTTP contract, since a hub is built
against it. Say so in the footer rather than only in the type.

```
fix(http_server): refuse a chunked request rather than misreading it

Transfer-Encoding: chunked was neither implemented nor rejected, so the
framing was read as though it were the body. A 501 says plainly that this
server does not do chunked, which is true and is all a client needs.

Fixes: #14
```

### Enable the hook

Once per clone:

```bash
git config core.hooksPath .githooks
git config commit.template .gitmessage
```

`.githooks/commit-msg` rejects a message that does not fit — a plain POSIX
shell script, identical to the one the sibling repositories use, so nothing
here depends on a toolchain this repository does not otherwise need. CI runs
that same file over every commit in a pull request.

Note that `.github/workflows/commit-messages.yml` is the only workflow that
checks anything about a change so far: there is still no build or static
analysis in CI. That is tracked separately and this does not stand in for it.

### Why this changed

The history before this point uses capitalised, prefix-free subjects in Git's
own style. Those commits are left alone: rewriting them would change every
hash and break the links from issues and pull requests. The log therefore has
a visible seam, which is the honest cost of the change.

What it buys is release automation. This repository has no version anywhere and
no tags at all, which for firmware is the more awkward gap of the three — a
device in a case cannot be asked what it is running unless the build knows.
`.github/workflows/release.yml` derives a version, a tag and a changelog from
the commit types, tracking the version in `version.txt`.

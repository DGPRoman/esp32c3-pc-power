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

A message that reads well is not evidence that the change is right, and this
gate does not pretend otherwise. `.github/workflows/ci.yml` is what builds the
firmware, fails the run on a warning from this repository's own sources, and
runs the host tests under the sanitisers.

### Why this changed

The history before this point uses capitalised, prefix-free subjects in Git's
own style. Those commits are left alone: rewriting them would change every
hash and break the links from issues and pull requests. The log therefore has
a visible seam, which is the honest cost of the change.

The convention was adopted for release automation, and that automation has since
been removed. release-please can only open its pull request if the repository
allows GitHub Actions to create and approve pull requests — a permission that
also lets a workflow approve one, which is wider than the automation was worth on
a repository whose pull requests are reviewed by hand anyway.

What the convention still buys is a log that says what each change *is* rather
than only what it touched, and a history a tool can read. Tags and a changelog
can be derived from it later, or the automation restored behind a token of its
own, without rewriting anything a second time.

This repository is left with no version anywhere and no tags at all, which for
firmware is the more awkward gap of the three: a device in a case cannot be asked
what it is running unless the build knows. That is now an open problem rather than
a solved one.

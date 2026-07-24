# CLAUDE.md

This is Shadowlos's **local fork** of [throneproj/Throne](https://github.com/throneproj/Throne).

It is **never** intended to be merged upstream. Nothing here needs to be written
with upstream acceptance in mind — don't shape changes around what upstream
would take.

We do keep it current: pull new upstream releases in periodically and replay our
local changes on top. `dev` is the product branch, and it is rebased onto the new
upstream base rather than merged, so our commits stay a readable stack at the tip.

See [FORK.md](FORK.md) for the bump procedure, the bump log (which upstream base
we're on, and the previous one), and the per-feature conflict-risk map.

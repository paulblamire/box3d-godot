# Fork maintenance

This fork keeps Box3D itself unchanged and adds a narrow Godot binding for
offline-baked compound collision. The binding owns and validates serialized
`b3CompoundData`, exposes it as `Box3DBakedCollision`, and attaches it through
`b3CreateBakedCompoundShape`.

The ordinary build instructions remain in [`godot/README.md`](godot/README.md).
This file only records the fork-specific workflow.

## Branches and provenance

- `main` should remain a fast-forward mirror of `Stink-O/box3d-godot`.
- `twok/offline-baked-compound` contains the production patch. Its initial base
  is upstream tag `v0.6.0`, commit
  `26d5ab2bbffbc97900289a845caf287791c5408d`.
- A consuming project must pin an exact fork commit and record the hash of the
  built library. Do not use a moving branch name as asset provenance.
- Do not overwrite an old, deployed patch commit when adopting a new upstream
  version. Create and test a new candidate branch so old assets remain
  reproducible.

## Clone and build on Windows

```powershell
git clone --recurse-submodules https://github.com/paulblamire/box3d-godot.git
Set-Location box3d-godot
git remote add upstream https://github.com/Stink-O/box3d-godot.git
git fetch upstream --tags
git switch twok/offline-baked-compound
git submodule update --init --recursive

Set-Location godot
py -3 -m SCons target=template_debug
py -3 -m SCons target=template_release
```

The Windows libraries are written under `godot/demo/addons/box3d/bin/`. The
first build also builds `godot-cpp` and can take several minutes.

Run the fork-specific smoke after the debug build:

```powershell
godot --headless --path demo --import
godot --headless --path demo --script res://tests/test_baked_collision.gd
```

The import step lets a fresh clone discover the extension before GDScript is
parsed. Success ends with `BAKED_COLLISION_SMOKE` and `"passed":true`. The
normal demo checks in `godot/README.md` should also pass before a new build is
consumed.

## Bring in a later upstream release

First update the mirror branch without mixing the patch into it:

```powershell
git fetch upstream --tags
git switch main
git merge --ff-only upstream/main
git push origin main
```

Then create a candidate from the upstream tag that will actually be shipped.
Cherry-pick the fork-only commits from the current patch branch, oldest first:

```powershell
$upstreamTag = "vNEXT" # replace with the release being evaluated
$forkCommits = @(git rev-list --reverse v0.6.0..twok/offline-baked-compound)
git switch -c "twok/offline-baked-compound-$upstreamTag" $upstreamTag
git cherry-pick $forkCommits
git submodule update --init --recursive
```

Resolve conflicts against the new binding API; do not modify Box3D collision or
solver behaviour merely to preserve the wrapper. Build both targets, run the
fork smoke and the normal demo checks, then run the consuming project's vehicle
and streamed-collision gates. Only after those pass should that project update
its pinned commit and library hash.

If upstream gains an equivalent resource API, prefer migrating to it and
retiring this patch instead of maintaining two implementations.

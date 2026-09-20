# Independent Director and ToolBook development

`master` stays at `e453f61d3b6d1130c0222ee5a9730866fd05e20e`.
The two game series and this tooling series are reviewed in separate open PRs.

| Branch | Purpose |
| --- | --- |
| `codex/seven-witches` | Director behavior fixes and Russian Seven Witches detection/configuration |
| `codex/toolbook` | Recovered ToolBook development, including isolated engine-specific diagnostic commits |
| `codex/devtools` | Headless backend, Director bridge, runner, scenarios, research materials and provenance |

Edit and commit engine source in its own worktree. A cumulative patch export is
not part of building, running or saving work. The ToolBook engine does not depend
on the Director game changes. Its internal scripted input and tracing are
separate commits because the engine itself is absent from the common base.

## Worktrees and builds

From a clone of the fork:

```sh
git fetch origin
git worktree add ../seven-witches codex/seven-witches
git worktree add ../toolbook codex/toolbook
git worktree add ../devtools codex/devtools
python3 ../devtools/devtools/run.py build --engine director \
  --source ../seven-witches --build ../build/seven-witches
python3 ../devtools/devtools/run.py build --engine toolbook \
  --source ../toolbook --build ../build/toolbook
```

The build uses ScummVM's normal configure/make toolchain. On the migration Mac,
Apple Clang and the existing Homebrew SDL2/dependencies were sufficient. A
fresh machine should follow ScummVM's platform build documentation. The runner
uses only the Python standard library. Optional historical image/disassembly
tools need Pillow and Capstone respectively.

For headless validation, create a temporary integration worktree for each game
and merge `codex/devtools` into that temporary branch. Keep the game branch as
the first parent; the other game's branch is not needed.

```sh
git worktree add -b codex/check-seven-witches ../check-seven-witches codex/seven-witches
git -C ../check-seven-witches merge --no-ff codex/devtools
git worktree add -b codex/check-toolbook ../check-toolbook codex/toolbook
git -C ../check-toolbook merge --no-ff codex/devtools
```

The initial Director integration has one conflict between adjacent constructor
initializers in `engines/director/director.cpp`: retain `_cdDriveLetter = 0;`
followed by `_inputScriptPos = 0;`, `_inputScriptLoaded = false;` and
`_debugBridge = nullptr;`. Both changes are necessary. Finish that temporary
merge with `git add` and `git commit`, then build the integration worktree into
its own build directory. Further conflicts after future engine changes need
individual review.

## Isolated runs

Game files stay outside Git. Supply their directory explicitly. Each output
directory must be new; it gets its own saves, configuration, scenario copy,
log, frames, source SHA, binary SHA-256 and result. The runner terminates only
the process it started.

```sh
python3 ../devtools/devtools/run.py run --engine toolbook \
  --source ../check-toolbook --build ../build/check-toolbook \
  --data /path/to/bashnya --output ../runs/toolbook-001 \
  --input ../devtools/devtools/scenarios/toolbook-new-game.txt --seconds 40
python3 ../devtools/devtools/run.py run --engine director \
  --source ../check-seven-witches --build ../build/check-seven-witches \
  --data /path/to/tivola --output ../runs/witches-001 --scenario tour
```

The tour drives normal mouse events from language selection through the city,
all seven locations and returns. It uses the local Director bridge. The default
dummy audio driver exercises decoding/playback without physical audio output;
`--audio` selects the host audio driver. For bridge inspection after a tour,
`--linger 180` keeps the process available for three minutes.

`result.json` distinguishes runner failures and fatal engine errors. A timed run
with frames only proves that the engine stayed active; evaluate its recorded
barrier and page sequence against the scenario. ToolBook's migration expectation
is `mainMenu`, then `OpenScript opcode 37 @0x74e14` after the new-game click.
That barrier remains unsupported, so this is not a completed-game claim.

## History and verification

[PROVENANCE.md](migration/PROVENANCE.md) explains the split and maps all original
patches/nested commits to source commits. `migration/provenance.json` contains
the complete machine-readable mapping. To check an integration of all three
series against the pre-migration sources:

```sh
python3 devtools/migration/verify.py --repository . --combined <integration-commit>
```

`workbench/` preserves the old tools and research ledger in their original
chronological commits. `HISTORICAL-WORKFLOW.md` and `STATE.historical.md` describe
the old setup and are archival evidence. Existing historical shell wrappers
still assume the old layout and include known runner issues; use `run.py` for
the active workflow. The prior cumulative patches and machine-local MCP
configuration are available in the archived workbench history.

Original tips are saved under `archive/2026-09-20/*` tags. The original local
workbench and nested source directory are also retained, together with complete
file backups and Git bundles. The remaining engine/tooling backlog is tracked
in [issue #1](https://github.com/kfastov/scummvm/issues/1).

# Validation of the 2026-09-20 migration

The preserved source state is tree
`de4c364b004425a8c19e42d7dbc2baaea43c1e92`, based on
`e453f61d3b6d1130c0222ee5a9730866fd05e20e`.

- Independently reconstructed all 48 cumulative patches; every recorded new
  blob hash matched. The last patch equals the original dirty source worktree.
- Recovered all 28 nested ToolBook states, including five missing from patch
  history. Split the mixed object/input checkpoint into two commits without
  changing its resulting tree. All 74 ToolBook checkpoint records (46 patch
  records plus 28 nested records) are represented.
- The merged source series match all 24,449 files of the original source tree.
  The 33 modified/added files are listed in `provenance.json`. New workbench
  documentation and runner files are the only additional material.
- Clean, separate configure/make builds succeeded for Director alone, ToolBook
  alone, Director plus development tools, and ToolBook plus development tools.
  Each enabled only its own engine. Builds used Apple Clang, SDL2 and the
  existing macOS dependencies. The linker emitted its existing duplicate SDL2
  library warning; there were no build errors.
- Director: normal input reached language selection, save-slot selection,
  introduction, city, Turm, Kneipe, Markt, Ecke, Salon, Laden and Spielpl,
  returning to the city after each location. Seven screenshots were captured;
  every location's sound sample had an active mixer channel. No fatal Lingo
  errors occurred. Physical speaker output was not tested (SDL dummy audio).
- Director drag regression in Spielpl: hit testing selected gift sprite 103 at
  `(353,455)`; a 1.8-second drag to `(440,370)` held the mouse button and moved
  sprite 120 under the cursor. The original gift moved offscreen while dragging
  and returned on release; `the clickOn` remained 103 after mouseUp. The engine
  continued running without the former out-of-range error. This checks dragging
  and release handling, not whether that gift is appropriate for the character.
- ToolBook: the 40-second new-game scenario produced 36 frames, reached
  `mainMenu`, played startup/menu/click WAVs and reached the preserved barrier
  `OpenScript opcode 37 @0x74e14 пока не реализован`. No new fatal errors.
- Inspected the rendered ToolBook menu and Director Salon image. This is a
  regression check of known behavior, not complete game or cross-platform QA.

The first drag probe attempted nested `eval` while the game was in its
`stillDown` handler; the bridge returned no value, so that probe was inconclusive.
The repeated probe used read-only state/sprite inspection while dragging and
evaluated Lingo after release. Its raw evidence and all run identities are in
`validation.json`. No engine source was changed to pass either check.

Historical workbench files preserve their original bytes, including Windows
CRLF in `guest-tools/NOACCEL.REG` and an old trailing blank line in ledger/0049.
Whitespace checks pass for the source series and new tooling files when those
unaltered historical materials are excluded.

Complete local backups were checked by SHA-256: 65,365 files, 10,708,093,608
bytes. Both Git bundles passed `git bundle verify`. The original workbench
branch, nested source branch and four dirty ToolBook source files are retained.
Archive tags preserve the old remote branch tips and original local histories.

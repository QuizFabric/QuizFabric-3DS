# Automated production releases

Every push to `main` and every manual run of the **Production pre-release**
workflow builds the production 3DS app and publishes a GitHub pre-release with
the `.3dsx` attached. It also attaches the `.cia` when `makerom` and
`bannertool` are available and CIA packaging succeeds; otherwise it publishes
the `.3dsx` alone and notes the missing CIA in the release description.

The app's displayed build number is the workflow run number plus 50. Before
compiling, the workflow updates `source/build_number.h` and pushes that change
to the branch that triggered the run. The workflow token needs permission to
write repository contents, and repository branch protection must allow that
push.

The release title is `build <number>`, and its notes list commits since the
most recent GitHub release. The release tag is `build-<number>`.

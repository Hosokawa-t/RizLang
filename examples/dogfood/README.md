# Dogfood apps

Small, tool-shaped Riz programs used to pressure-test everyday workflows.
Run them from the repository root.

```bash
riz examples/dogfood/task_board.riz
riz examples/dogfood/expense_report.riz
riz examples/dogfood/release_notes.riz
riz examples/dogfood/fixture_pkg_audit.riz
riz examples/dogfood/task_board.riz --status done
riz examples/dogfood/expense_report.riz --category cloud
riz examples/dogfood/release_notes.riz --kind fix
riz examples/dogfood/fixture_pkg_audit.riz --root tests
```

What each one exercises:

- `task_board.riz`: structs, `read_tsv`, `parse_flags`, list/dict processing, simple reporting
- `expense_report.riz`: TSV parsing, numeric aggregation, sorting, category filtering
- `release_notes.riz`: grouped text generation with lightweight CLI filtering
- `fixture_pkg_audit.riz`: `glob`, `walk_dir`, `join_path`, and file I/O against real package fixtures under `tests/`

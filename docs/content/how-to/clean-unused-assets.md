+++
title = 'Clean unused assets'
weight = 9
math = false
+++

# Clean unused assets

Find assets nothing in the scene uses and delete the ones you confirm. Cleanup is deliberate:
the tool reports candidates and you confirm what to remove. Nothing is ever auto-deleted.

You need an active project. The scan classifies by reachability from the active scene, so open
the scene whose assets you want to keep before you start.

## Steps

1. **Get the report.** It is a dry run — it never deletes:
   ```sh
   sa clean-assets
   ```
   Each candidate carries a category and a reason. Keep a candidate by passing its id to
   `--exclude` (an explicit root that is always kept).

2. **Read the categories.** They are reported separately, because they are different problems:
   - **unused** — indexed but unreachable from the active scene. The only deletable category.
   - **review** — referenced only by something a static scan can't follow (a script field). Never
     deleted automatically; confirm by hand.
   - **broken** — a scene or material points at an id with no catalog row. A diagnostic, not a
     deletion target — fix the reference.
   - **orphaned** — a recognized file on disk with no catalog row (the filesystem scan makes this
     rare).

3. **Commit to version control first.** Deletion is irreversible.

4. **Delete only what you confirmed.** `confirm` is required; only `unused` ids are removed, and a
   rescan then surfaces anything the deletion newly orphaned:
   ```sh
   sa delete-unused 12345 67890 --confirm
   ```

## Verify

- `sa clean-assets` no longer lists the deleted ids.
- `sa list-assets` — the in-use models and their sub-assets are still present.
- The model you instantiated still renders: `sa screenshot viewport /tmp/after-clean.png`.

## In the code

| What | File | Symbols |
|---|---|---|
| Classify by reachability | `assets/src/manage.rs` | `analyze_clean`, `build_dependency_graph` |
| Script-referenced (review) | `assets/src/manage.rs` | `collect_script_referenced_ids` |
| Confirm-gated delete + rescan | `assets/src/manage.rs` | `delete_unused` |
| Commands | `control/src/commands_asset.rs` | `clean-assets`, `delete-unused` |

> [!WARNING]
> `delete-unused` removes files and is irreversible. It refuses without `confirm`, deletes only
> ids classified `unused`, and logs every removal — but commit first so a mistake is recoverable.

## Related

- [The .smodel container](../../explanations/geometry-and-assets/smodel-container/)
- [Asset server & catalog](../../explanations/geometry-and-assets/asset-server-and-catalog/)

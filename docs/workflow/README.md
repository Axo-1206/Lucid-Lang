# Patch Workflow Guide

This document explains how to apply, verify, and manage patches in the Lucid compiler repository.

---

## Overview

Patches are stored in the `.patches/` directory at the root of the repository. Each patch file is named with a sequential number and a descriptive name:

```
.patches/
├── 001-unify-scope-exit-cleanup.patch
├── 002-runtime-concurrency.patch
└── ...
```

---

## Prerequisites

- **Git** (version 2.20 or later) – required for applying patches
- **VS Code** (optional) – for reviewing changes before applying

---

## Quick Start: Apply a Patch

### Method 1: Using Git (Recommended)

```bash
# 1. Navigate to the repository root
cd /path/to/lucid

# 2. Check if the patch applies cleanly
git apply --check .patches/001-unify-scope-exit-cleanup.patch

# 3. Apply the patch
git apply .patches/001-unify-scope-exit-cleanup.patch

# 4. Verify the changes
git diff
```

### Method 2: Using Git with 3-Way Merge (If Conflicts Occur)

```bash
# Apply with 3-way merge (handles conflicts automatically)
git apply --3way .patches/001-unify-scope-exit-cleanup.patch

# If conflicts appear, resolve them in VS Code:
# - Open the conflicted file
# - VS Code will show diff with accept/reject buttons
# - Accept the changes you want
```

### Method 3: Using the `patch` Command (If Git is Not Available)

```bash
# Apply the patch
patch -p1 < .patches/001-unify-scope-exit-cleanup.patch

# Check if it applied cleanly
patch -p1 --dry-run < .patches/001-unify-scope-exit-cleanup.patch
```

---

## Review a Patch Before Applying

### Using VS Code

```bash
# Open the patch file in VS Code
code .patches/001-unify-scope-exit-cleanup.patch

# VS Code will show syntax highlighting for the diff
# You can see exactly what changes are made
```

### Using `git apply --stat`

```bash
# Show a summary of changes
git apply --stat .patches/001-unify-scope-exit-cleanup.patch

# Output:
# src/codegen/support/LiveVariableTracker.hpp |  5 +++
# src/codegen/context/CodeGenContext.hpp      | 87 ++++++++++++++++-----
# src/codegen/CodeGenStmt.cpp                 | 38 +++++++------
# 3 files changed, 102 insertions(+), 28 deletions(-)
```

### Using `git apply --numstat`

```bash
# Show detailed change counts per file
git apply --numstat .patches/001-unify-scope-exit-cleanup.patch
```

---

## Troubleshooting

### Error: "patch does not apply"

This means the file has changed since the patch was created.

**Solution 1 – Try 3-way merge:**

```bash
git apply --3way .patches/001-unify-scope-exit-cleanup.patch
```

**Solution 2 – Show rejected hunks:**

```bash
git apply --reject .patches/001-unify-scope-exit-cleanup.patch
# This creates .rej files showing what didn't apply
```

**Solution 3 – Apply manually:**

1. Open the `.rej` file to see what changed
2. Manually edit the original file
3. Delete the `.rej` files

### Error: "patch: **** malformed patch"

The patch file might have been corrupted (e.g., Windows line endings).

**Solution:**

```bash
# Convert line endings to Unix format
dos2unix .patches/001-unify-scope-exit-cleanup.patch

# Or on Windows (PowerShell):
Get-Content .patches\001-unify-scope-exit-cleanup.patch -Raw | Set-Content .patches\001-unify-scope-exit-cleanup.patch
```

### Error: "git apply: unrecognized input"

Make sure the patch file starts with `From:` or `diff --git`.

**Solution:** Verify the file format:

```bash
head -n 5 .patches/001-unify-scope-exit-cleanup.patch
# Should show:
# From: AI Assistant <ai@lucid-lang.org>
# Date: ...
# Subject: [PATCH] ...
```

---

## Reverting a Patch

### If Applied with Git

```bash
# Revert the last applied patch
git revert HEAD

# Or revert a specific patch
git revert <commit-hash>
```

### If Applied with `patch`

```bash
# Create a reverse patch
patch -p1 -R < .patches/001-unify-scope-exit-cleanup.patch

# Or use the `-R` flag
patch -p1 -R --dry-run < .patches/001-unify-scope-exit-cleanup.patch
```

---

## Managing Multiple Patches

### Apply All Patches in Order

```bash
# Apply all patches in numerical order
for patch in $(ls -1 .patches/*.patch | sort); do
    echo "Applying: $patch"
    git apply --check "$patch" || { echo "Failed: $patch"; break; }
    git apply "$patch"
done
```

### View All Patches

```bash
# List all patches
ls -la .patches/*.patch

# Show summary of each patch
for patch in .patches/*.patch; do
    echo "=== $patch ==="
    git apply --stat "$patch"
    echo ""
done
```

---

## Creating a New Patch

### From Uncommitted Changes

```bash
# Stage your changes
git add <files>

# Create a patch
git diff --staged > .patches/XXX-description.patch

# Or with full commit message format
git format-patch -1 -o .patches/
```

### From Committed Changes

```bash
# Create a patch from the last commit
git format-patch -1 -o .patches/

# Create a patch from a specific commit
git format-patch -1 <commit-hash> -o .patches/
```

---

## VS Code Integration

### Recommended Extensions

1. **GitLens** – Shows who changed what, inline blame
2. **Patch Viewer** – Better syntax highlighting for patches
3. **Diff & Merge** – Visual diff with accept/reject buttons

### Quick Patch Review in VS Code

```bash
# Open patch in VS Code
code .patches/001-unify-scope-exit-cleanup.patch

# Right-click → "Open Diff with Original" (if available)
# Or use the built-in diff viewer:
code --diff .patches/001-unify-scope-exit-cleanup.patch <(git show HEAD:file)
```

---

## Summary Table

| Command                         | Purpose                          |
| ------------------------------- | -------------------------------- |
| `git apply --check file.patch`  | Check if patch applies cleanly   |
| `git apply file.patch`          | Apply the patch                  |
| `git apply --3way file.patch`   | Apply with 3-way merge           |
| `git apply --stat file.patch`   | Show summary of changes          |
| `git apply --reject file.patch` | Apply what you can, save rejects |
| `patch -p1 < file.patch`        | Apply with `patch` command       |
| `patch -p1 -R < file.patch`     | Revert a patch                   |
| `git revert HEAD`               | Revert the last commit           |

---

## Notes

1. **Always backup** before applying patches, especially if you have uncommitted changes
2. **Test patches** in a clean branch first: `git checkout -b test-patches`
3. **Document the patch** in the commit message with the task reference
4. **Update `Architecture.md`** if the patch changes the architecture
```

---

## 4. How to Apply the Patch

### Step 1: Create the patch file

```bash
# Create the .patches directory if it doesn't exist
mkdir -p .patches

# Create the patch file
# Copy the entire patch content from above into this file
# You can use VS Code to paste it
code .patches/001-unify-scope-exit-cleanup.patch
```

### Step 2: Check the patch

```bash
# See what the patch will change
git apply --stat .patches/001-unify-scope-exit-cleanup.patch

# Check if it applies cleanly
git apply --check .patches/001-unify-scope-exit-cleanup.patch
```

### Step 3: Apply the patch

```bash
# Apply it
git apply .patches/001-unify-scope-exit-cleanup.patch

# Or with 3-way merge if you expect conflicts
git apply --3way .patches/001-unify-scope-exit-cleanup.patch
```

### Step 4: Verify and Commit

```bash
# See what changed
git diff

# Build and test
make build

# Commit the changes
git add .
git commit -m "Unify scope-exit cleanup with non-destructive unwind

This implements Task 1 from RedesignPlan.md.

- Add BlockStmtAST* block to LiveVariableTracker
- Add scopeDepth to LoopInfo
- Factor emitCleanupForTracker from emitScopeExitCleanup
- Add emitUnwindTo (non-destructive)
- Update lowerBlockStmt, lowerReturnStmt, lowerBreakStmt, lowerContinueStmt
- Remove old emitScopeExitCleanup public entry point

#scope_exit callbacks now run BEFORE implicit cleanup (closures, arrays, strings)
so callbacks can safely access heap-backed locals.

Break/continue now unwind to loop->scopeDepth without popping the loop's
own scope."
```

---

## File Structure After Creation

```
lucid/
├── .patches/
│   └── 001-unify-scope-exit-cleanup.patch   # ← New patch file
├── docs/
│   └── workflow/
│       └── README.md                        # ← New instructions
├── src/
│   └── ...
└── ...
```

---

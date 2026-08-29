#!/usr/bin/env python3
"""
PatchRunner v4.0 - Clean Patch Generator

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📖 HOW TO USE:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. BASIC USAGE:
   python PatchRunner.py patches/your-patch.patch

2. WITH OPTIONS:
   python PatchRunner.py patches/your-patch.patch --verbose --no-color

3. EXAMPLES:
   # Apply a patch with verbose output
   python PatchRunner.py patches/001-unify-scope-exit-cleanup.patch --verbose

   # Apply without diagnostic noise
   python PatchRunner.py patches/001-unify-scope-exit-cleanup.patch --no-diag

   # Force apply even with unrelated changes
   python PatchRunner.py patches/001-unify-scope-exit-cleanup.patch --force

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
"""

import os
import re
import sys
import subprocess
import tempfile
import shutil
import difflib
from pathlib import Path
from typing import List, Tuple, Optional, Dict, Set, Any

# ============================================================================
# ⚙️  CONFIGURATION - Edit these to customize behavior
# ============================================================================

# ─── File Processing ──────────────────────────────────────────────────────

# File extensions to process (others will be ignored)
ALLOWED_EXTENSIONS = ['.hpp', '.cpp', '.h', '.c', '.cc', '.cxx', '.hxx']

# Directories to ignore when checking git status
IGNORE_DIRS = ['docs/', 'tests/', 'PatchRunner.py', '.patches/']

# ─── Git Behavior ─────────────────────────────────────────────────────────

# If True, ignore unrelated changes in git status (don't show warnings)
IGNORE_UNRELATED_CHANGES = True

# ─── Script Behavior ──────────────────────────────────────────────────────

# Maximum attempts to fix the patch
MAX_RETRIES = 3

# VS Code command (change to 'code-insiders' if using Insiders)
VSCODE_CMD = 'code'

# Verbose output
VERBOSE = True

# Color output
USE_COLORS = True

# Diagnostic mode
DIAGNOSTIC_MODE = True

# ─── Color Codes ──────────────────────────────────────────────────────────

if USE_COLORS:
    COLORS = {
        "INFO": "\033[94m",
        "SUCCESS": "\033[92m",
        "WARNING": "\033[93m",
        "ERROR": "\033[91m",
        "DIAG": "\033[90m",
        "ACTION": "\033[96m",
        "HELP": "\033[95m",
        "RESET": "\033[0m"
    }
else:
    COLORS = {
        "INFO": "",
        "SUCCESS": "",
        "WARNING": "",
        "ERROR": "",
        "DIAG": "",
        "ACTION": "",
        "HELP": "",
        "RESET": ""
    }

# ============================================================================
# MAIN CLASS
# ============================================================================

class PatchRunner:
    def __init__(self, repo_root: str, patch_path: str, force: bool = False):
        self.repo_root = Path(repo_root)
        self.patch_path = Path(patch_path)
        self.force = force
        self.verbose = VERBOSE
        self.diagnostic = DIAGNOSTIC_MODE
        self.vscode_cmd = VSCODE_CMD
        self.ignore_unrelated = IGNORE_UNRELATED_CHANGES
        self.ignore_dirs = IGNORE_DIRS
        self.allowed_extensions = ALLOWED_EXTENSIONS
        self.action_log: List[str] = []
        self.target_files: Set[str] = set()
        
    def log(self, msg: str, level: str = "INFO"):
        if not self.verbose and level not in ["ERROR", "ACTION", "HELP"]:
            return
        prefix = COLORS.get(level, "")
        reset = COLORS["RESET"]
        print(f"{prefix}[{level}]{reset} {msg}")
        
    def log_action(self, msg: str):
        self.action_log.append(msg)
        self.log(f"▶ {msg}", "ACTION")
        
    def log_diagnostic(self, msg: str):
        if self.diagnostic:
            self.log(f"🔍 {msg}", "DIAG")
    
    def log_help(self):
        """Display help information."""
        self.log("\n" + "="*70, "HELP")
        self.log("📖 PATCHRUNNER HELP", "HELP")
        self.log("="*70, "HELP")
        self.log("Usage: python PatchRunner.py <patch_file> [options]", "HELP")
        self.log("", "HELP")
        self.log("Options:", "HELP")
        self.log("  --verbose     Show detailed output", "HELP")
        self.log("  --no-color    Disable colored output", "HELP")
        self.log("  --no-diag     Disable diagnostic logging", "HELP")
        self.log("  --force       Force apply even with unrelated changes", "HELP")
        self.log("  --help        Show this help message", "HELP")
        self.log("", "HELP")
        self.log("Examples:", "HELP")
        self.log("  python PatchRunner.py patches/001-unify-scope-exit-cleanup.patch", "HELP")
        self.log("  python PatchRunner.py patches/001-unify-scope-exit-cleanup.patch --verbose", "HELP")
        self.log("="*70, "HELP")
    
    def should_ignore_file(self, filepath: str) -> bool:
        """Check if a file should be ignored."""
        for ignore_dir in self.ignore_dirs:
            if ignore_dir in filepath:
                return True
        ext = Path(filepath).suffix
        if ext and ext not in self.allowed_extensions:
            return True
        return False
    
    def get_affected_files_from_patch(self, patch_content: str) -> Set[str]:
        """Extract file paths from the patch."""
        files = set()
        for line in patch_content.split('\n'):
            if line.startswith('--- a/'):
                filepath = line[6:].strip()
                files.add(filepath)
            elif line.startswith('diff --git a/'):
                match = re.search(r'diff --git a/(.+?) b/', line)
                if match:
                    files.add(match.group(1))
        return files
    
    def check_git_status(self, target_files: Set[str]) -> Tuple[bool, List[str]]:
        """Check git status and filter out unrelated changes."""
        self.log_diagnostic("Checking git status...")
        
        result = subprocess.run(
            ['git', 'status', '--porcelain'],
            cwd=self.repo_root,
            capture_output=True,
            text=True
        )
        
        if not result.stdout.strip():
            self.log_diagnostic("  Working directory is clean")
            return False, []
        
        modified_files = []
        
        for line in result.stdout.strip().split('\n'):
            if not line:
                continue
            status = line[:2]
            filepath = line[3:].strip()
            
            # Check if this file is related to our target
            is_related = False
            for target in target_files:
                if target in filepath or Path(filepath).name in target:
                    is_related = True
                    break
            
            if is_related:
                modified_files.append(f"{status} {filepath}")
            elif not self.should_ignore_file(filepath) and not self.ignore_unrelated:
                modified_files.append(f"{status} {filepath}")
        
        if modified_files:
            self.log(f"  Related files with changes:", "INFO")
            for f in modified_files:
                self.log(f"    {f}", "INFO")
        
        return len(modified_files) > 0, modified_files
    
    def parse_patch_to_edits(self, patch_content: str) -> Dict[Path, List[Dict[str, Any]]]:
        """
        Parse the patch and extract edits with proper line preservation.
        """
        self.log_diagnostic("Parsing patch to extract edits...")
        
        edits: Dict[Path, List[Dict[str, Any]]] = {}
        current_file = None
        current_hunk = None
        hunks = []
        
        lines = patch_content.split('\n')
        self.log_diagnostic(f"  Patch has {len(lines)} lines")
        
        i = 0
        while i < len(lines):
            line = lines[i]
            
            if line.startswith('--- a/'):
                current_file = line[6:].strip()
                self.log_diagnostic(f"  Found file: {current_file}")
                current_hunk = None
                
            elif line.startswith('+++ b/'):
                pass
                
            elif line.startswith('@@ '):
                match = re.match(r'@@ -(\d+),(\d+) \+(\d+),(\d+) @@(.*)', line)
                if match:
                    old_start = int(match.group(1))
                    old_count = int(match.group(2))
                    new_start = int(match.group(3))
                    new_count = int(match.group(4))
                    
                    current_hunk = {
                        'old_start': old_start,
                        'old_count': old_count,
                        'new_start': new_start,
                        'new_count': new_count,
                        'lines': [],
                        'context': match.group(5).strip()
                    }
                    hunks.append((current_file, current_hunk))
                    self.log_diagnostic(f"    Hunk: -{old_start},{old_count} +{new_start},{new_count}")
                    
            elif current_hunk is not None:
                current_hunk['lines'].append(line)
                
            i += 1
        
        self.log_diagnostic(f"  Found {len(hunks)} hunks")
        
        for filepath, hunk in hunks:
            if not filepath:
                continue
                
            path = Path(filepath)
            if path not in edits:
                edits[path] = []
            
            old_lines = []
            new_lines = []
            context_lines = []
            
            for line in hunk['lines']:
                if line.startswith('-'):
                    old_lines.append(line[1:])
                elif line.startswith('+'):
                    new_lines.append(line[1:])
                elif line.startswith(' '):
                    context_lines.append(line[1:])
                elif line.startswith('@@'):
                    pass
            
            if old_lines or new_lines:
                edits[path].append({
                    'old_start': hunk['old_start'],
                    'old_count': len(old_lines),
                    'new_lines': new_lines,
                    'old_lines': old_lines,
                    'context_lines': context_lines
                })
                self.log_diagnostic(f"    Edit: {path} at line {hunk['old_start']} (-{len(old_lines)} +{len(new_lines)})")
        
        return edits
    
    def apply_edits_to_file(self, filepath: Path, file_edits: List[Dict[str, Any]]) -> bool:
        """
        Apply the edits to a file while preserving indentation and structure.
        """
        full_path = self.repo_root / filepath
        self.log_diagnostic(f"  Applying {len(file_edits)} edits to {filepath}")
        
        if not full_path.exists():
            self.log(f"  ❌ File not found: {full_path}", "ERROR")
            return False
        
        try:
            with open(full_path, 'r', encoding='utf-8') as f:
                content = f.readlines()
            self.log_diagnostic(f"    File has {len(content)} lines")
            
            # Sort edits in reverse order (bottom to top)
            edits_sorted = sorted(file_edits, key=lambda e: e['old_start'], reverse=True)
            
            for idx, edit in enumerate(edits_sorted):
                start = edit['old_start'] - 1
                end = start + edit['old_count']
                
                self.log_diagnostic(f"    Edit {idx+1}: line {start+1}, count {edit['old_count']}")
                
                # Find the correct location with context
                found_line = self.find_location_with_context(content, edit)
                
                if found_line is not None:
                    # Apply the edit at the found location
                    end = found_line + edit['old_count']
                    if edit['new_lines']:
                        # Preserve indentation from the first old line if possible
                        indent = self.get_indentation(content, found_line)
                        if indent and edit['new_lines']:
                            new_lines = [indent + line.lstrip() if line.strip() else line 
                                        for line in edit['new_lines']]
                            content[found_line:end] = new_lines
                        else:
                            content[found_line:end] = edit['new_lines']
                    else:
                        # Removing lines
                        content[found_line:end] = []
                    self.log_diagnostic(f"      Applied edit at line {found_line+1}")
                else:
                    self.log(f"  ❌ Could not apply edit at line {start+1}", "ERROR")
                    return False
            
            # Write the modified content back
            with open(full_path, 'w', encoding='utf-8') as f:
                f.writelines(content)
            
            self.log_diagnostic(f"  ✅ Applied edits to {filepath}")
            return True
            
        except Exception as e:
            self.log(f"  ❌ Error applying edits to {filepath}: {e}", "ERROR")
            import traceback
            self.log_diagnostic(traceback.format_exc())
            return False
    
    def get_indentation(self, content: List[str], line_num: int) -> str:
        """Get the indentation of a line."""
        if line_num < len(content):
            match = re.match(r'^(\s*)', content[line_num])
            if match:
                return match.group(1)
        return ""
    
    def find_location_with_context(self, content: List[str], edit: Dict[str, Any]) -> Optional[int]:
        """
        Find the correct location using context lines.
        """
        old_lines = edit.get('old_lines', [])
        context_lines = edit.get('context_lines', [])
        
        if not old_lines and not context_lines:
            return edit['old_start'] - 1
        
        # Build search pattern
        search_pattern = []
        if context_lines:
            search_pattern.extend(context_lines)
        if old_lines:
            search_pattern.extend(old_lines)
        
        # Search for the pattern
        for i in range(len(content) - len(search_pattern) + 1):
            match = True
            for j, pattern_line in enumerate(search_pattern):
                if i + j < len(content):
                    if content[i + j].strip() != pattern_line.strip():
                        match = False
                        break
                else:
                    match = False
                    break
            if match:
                return i
        
        # Try fuzzy search
        if old_lines and old_lines[0].strip():
            for i, line in enumerate(content):
                if old_lines[0].strip() in line.strip():
                    return i
        
        return None
    
    def generate_clean_patch(self, target_files: Set[Path]) -> Optional[str]:
        """
        Generate a clean patch using git diff, only staging target files.
        """
        self.log_diagnostic(f"Generating clean patch for {len(target_files)} files...")
        
        try:
            # Only stage the target files
            for filepath in target_files:
                if filepath.exists():
                    self.log_diagnostic(f"  Staging {filepath}")
                    subprocess.run(
                        ['git', 'add', str(filepath)],
                        cwd=self.repo_root,
                        capture_output=True,
                        text=True
                    )
            
            # Generate patch
            self.log_diagnostic("  Running git diff --staged...")
            result = subprocess.run(
                ['git', 'diff', '--staged'],
                cwd=self.repo_root,
                capture_output=True,
                text=True
            )
            
            if result.returncode == 0 and result.stdout.strip():
                self.log_diagnostic(f"  Generated clean patch with {len(result.stdout.split(chr(10)))} lines")
                return result.stdout
            else:
                self.log_diagnostic("  No changes to stage?")
                return None
                
        except Exception as e:
            self.log_diagnostic(f"  Error generating patch: {e}")
            return None
    
    def show_diagnostics(self):
        """Show diagnostic summary."""
        self.log("\n" + "="*60, "DIAG")
        self.log("🔍 DIAGNOSTIC SUMMARY:", "DIAG")
        self.log("="*60, "DIAG")
        for action in self.action_log:
            self.log(f"  • {action}", "DIAG")
        self.log("="*60, "DIAG")
    
    def run(self):
        """Main execution."""
        self.log(f"PatchRunner v4.0 - Clean Patch Generator", "SUCCESS")
        self.log(f"Repo: {self.repo_root}", "INFO")
        self.log(f"Patch: {self.patch_path}", "INFO")
        self.log(f"Ignore unrelated changes: {self.ignore_unrelated}", "INFO")
        self.log("=" * 60, "INFO")
        
        # Read the malformed patch
        self.log_action("Reading patch file")
        try:
            with open(self.patch_path, 'r', encoding='utf-8') as f:
                patch_content = f.read()
            self.log_diagnostic(f"  Patch size: {len(patch_content)} bytes, {len(patch_content.split(chr(10)))} lines")
        except Exception as e:
            self.log(f"Failed to read patch: {e}", "ERROR")
            return False
        
        # Get target files from patch
        self.target_files = self.get_affected_files_from_patch(patch_content)
        self.log_diagnostic(f"  Target files: {self.target_files}")
        
        # Parse the patch to extract edits
        self.log_action("Extracting changes from patch")
        edits = self.parse_patch_to_edits(patch_content)
        
        if not edits:
            self.log("No edits found in patch!", "ERROR")
            return False
        
        self.log(f"Found edits for {len(edits)} files:", "INFO")
        for filepath, file_edits in edits.items():
            self.log(f"  • {filepath}: {len(file_edits)} hunks", "INFO")
        
        # Check git status (only showing related changes)
        self.log_action("Checking git status")
        has_related_changes, related_files = self.check_git_status(self.target_files)
        
        if has_related_changes:
            self.log(f"⚠️ Related files have uncommitted changes!", "WARNING")
            self.log(f"   These changes will be overwritten!", "WARNING")
            
            if not self.force:
                response = input("Continue? (y/N): ")
                if response.lower() != 'y':
                    self.log("Aborted by user", "ERROR")
                    return False
            else:
                self.log("Force mode enabled - continuing...", "INFO")
        
        # Create a backup branch
        self.log_action("Creating backup branch 'patch-backup'")
        subprocess.run(
            ['git', 'checkout', '-b', 'patch-backup'],
            cwd=self.repo_root,
            capture_output=True,
            text=True
        )
        
        # Apply the edits
        self.log_action("Applying edits to source files")
        success = True
        for filepath, file_edits in edits.items():
            self.log(f"  Processing {filepath}...", "INFO")
            if not self.apply_edits_to_file(filepath, file_edits):
                success = False
                self.log(f"  ❌ Failed to apply edits to {filepath}", "ERROR")
                break
            else:
                self.log(f"  ✅ Applied edits to {filepath}", "SUCCESS")
        
        if not success:
            self.log_action("Restoring from backup")
            self.log("Failed to apply edits, restoring backup...", "ERROR")
            subprocess.run(
                ['git', 'checkout', '--', '.'],
                cwd=self.repo_root,
                capture_output=True,
                text=True
            )
            subprocess.run(
                ['git', 'checkout', 'main'],
                cwd=self.repo_root,
                capture_output=True,
                text=True
            )
            subprocess.run(
                ['git', 'branch', '-D', 'patch-backup'],
                cwd=self.repo_root,
                capture_output=True,
                text=True
            )
            self.show_diagnostics()
            return False
        
        # Generate a clean patch
        self.log_action("Generating clean patch with git diff")
        clean_patch = self.generate_clean_patch(set(edits.keys()))
        
        if clean_patch:
            # Save the clean patch
            clean_path = self.patch_path.with_suffix('.clean.patch')
            with open(clean_path, 'w', encoding='utf-8') as f:
                f.write(clean_patch)
            self.log(f"✅ Clean patch saved to: {clean_path}", "SUCCESS")
            
            # Show the patch summary
            self.log("\n" + "="*60, "INFO")
            self.log("📊 Clean Patch Summary:", "SUCCESS")
            
            # Show stats from the clean patch
            lines = clean_patch.split('\n')
            for line in lines[:10]:
                if line.startswith(' src/') or line.startswith('---') or line.startswith('+++'):
                    self.log(f"  {line}", "INFO")
            self.log("\n" + "="*60, "INFO")
            
            # Check if the clean patch applies
            self.log_action("Checking if clean patch applies")
            check_result = subprocess.run(
                ['git', 'apply', '--check', str(clean_path)],
                cwd=self.repo_root,
                capture_output=True,
                text=True
            )
            
            if check_result.returncode == 0:
                self.log("✅ Clean patch applies cleanly!", "SUCCESS")
                
                # Apply the clean patch
                self.log_action("Applying clean patch")
                apply_result = subprocess.run(
                    ['git', 'apply', str(clean_path)],
                    cwd=self.repo_root,
                    capture_output=True,
                    text=True
                )
                
                if apply_result.returncode == 0:
                    self.log("✅ Patch applied successfully!", "SUCCESS")
                    
                    # Open VS Code
                    self.log_action("Opening VS Code for review")
                    self.log("Opening VS Code to review changes...", "INFO")
                    subprocess.run([self.vscode_cmd, '.'], cwd=self.repo_root)
                    
                    self.log("\n" + "="*60, "INFO")
                    self.log("📝 TO REVIEW CHANGES:", "SUCCESS")
                    self.log("  1. Press Ctrl+Shift+G to open Source Control", "INFO")
                    self.log("  2. Click each changed file to see the diff", "INFO")
                    self.log("  3. Use Accept (✓) or Reject (✗) buttons", "INFO")
                    self.log("  4. Stage and commit when ready", "INFO")
                    self.log("="*60, "INFO")
                    
                    self.show_diagnostics()
                    return True
                else:
                    self.log(f"❌ Failed to apply clean patch: {apply_result.stderr}", "ERROR")
                    # Try 3-way
                    self.log_action("Trying 3-way merge")
                    result_3way = subprocess.run(
                        ['git', 'apply', '--3way', str(clean_path)],
                        cwd=self.repo_root,
                        capture_output=True,
                        text=True
                    )
                    if result_3way.returncode == 0:
                        self.log("✅ Patch applied with 3-way merge!", "SUCCESS")
                        subprocess.run([self.vscode_cmd, '.'], cwd=self.repo_root)
                        return True
            else:
                self.log(f"⚠️ Clean patch check failed: {check_result.stderr[:200]}", "WARNING")
                self.log("You can still use the clean patch manually.", "INFO")
                self.log(f"  code {clean_path}", "INFO")
        else:
            self.log("Failed to generate clean patch", "ERROR")
            self.show_diagnostics()
            return False
        
        self.show_diagnostics()
        return False

# ============================================================================
# ENTRY POINT
# ============================================================================

def main():
    # Check for help flag first
    if '--help' in sys.argv or '-h' in sys.argv:
        runner = PatchRunner('.', '')
        runner.log_help()
        sys.exit(0)
    
    if len(sys.argv) < 2:
        print("Error: Missing patch file argument")
        print("Usage: python PatchRunner.py <patch_file> [options]")
        print("Try: python PatchRunner.py --help for more information")
        sys.exit(1)
    
    patch_path = sys.argv[1]
    global VERBOSE, USE_COLORS, DIAGNOSTIC_MODE, IGNORE_UNRELATED_CHANGES
    force = False
    
    for arg in sys.argv[2:]:
        if arg == '--verbose':
            VERBOSE = True
        elif arg == '--no-color':
            USE_COLORS = False
        elif arg == '--no-diag':
            DIAGNOSTIC_MODE = False
        elif arg == '--force':
            force = True
    
    repo_root = subprocess.run(
        ['git', 'rev-parse', '--show-toplevel'],
        capture_output=True,
        text=True
    ).stdout.strip()
    
    if not repo_root:
        print("Error: Not in a Git repository")
        sys.exit(1)
    
    runner = PatchRunner(repo_root, patch_path, force)
    success = runner.run()
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
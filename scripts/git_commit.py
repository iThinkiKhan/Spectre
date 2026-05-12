

#!/usr/bin/env python3
"""
git_commit.py  —  safe commit helper for Spectre on Windows/NTFS

Usage:
    python scripts/git_commit.py "your commit message"

Why this exists:
    Git's atomic lock-then-rename file writes corrupt files on this
    Windows NTFS filesystem when accessed from WSL/Linux. This script
    does all git work in /tmp (a native Linux filesystem), then copies
    the objects, refs, and index back to the real .git directory.

To see what changed before committing:
    python scripts/git_commit.py --status
    python scripts/git_commit.py --diff
"""

import subprocess, os, shutil, sys

WIN_GIT  = os.path.join(os.path.dirname(__file__), '..', '.git')
WIN_GIT  = os.path.realpath(WIN_GIT)
WIN_WORK = os.path.realpath(os.path.join(WIN_GIT, '..'))
TMP_GIT  = '/tmp/spectre_git'

CONFIG = """[core]
\trepositoryformatversion = 0
\tfilemode = true
\tbare = false
\tlogallrefupdates = true
[user]
\tname = Jim Schneider
\temail = jim.schneider.25@gmail.com
"""

def sync_to_tmp():
    """Copy .git to /tmp so we can work on a native Linux filesystem."""
    if os.path.exists(TMP_GIT):
        shutil.rmtree(TMP_GIT)
    shutil.copytree(WIN_GIT, TMP_GIT)
    # Write clean config and remove stale locks
    with open(os.path.join(TMP_GIT, 'config'), 'w') as f:
        f.write(CONFIG)
    for lock in ['config.lock', 'index.lock']:
        p = os.path.join(TMP_GIT, lock)
        if os.path.exists(p):
            os.remove(p)

def sync_from_tmp():
    """Copy objects, refs, and index back to the Windows .git."""
    for subdir in ['objects', 'refs']:
        src = os.path.join(TMP_GIT, subdir)
        dst = os.path.join(WIN_GIT, subdir)
        for root, dirs, files in os.walk(src):
            rel = os.path.relpath(root, src)
            dst_root = os.path.join(dst, rel)
            os.makedirs(dst_root, exist_ok=True)
            for f in files:
                src_f = os.path.join(root, f)
                dst_f = os.path.join(dst_root, f)
                shutil.copy2(src_f, dst_f)
    shutil.copy2(os.path.join(TMP_GIT, 'index'),
                 os.path.join(WIN_GIT, 'index'))
    # Always re-write a clean config
    with open(os.path.join(WIN_GIT, 'config'), 'w') as f:
        f.write(CONFIG)

def git(cmd, use_tmp=True):
    env = os.environ.copy()
    env.update({
        'GIT_DIR': TMP_GIT if use_tmp else WIN_GIT,
        'GIT_WORK_TREE': WIN_WORK,
        'GIT_AUTHOR_NAME': 'Jim Schneider',
        'GIT_AUTHOR_EMAIL': 'jim.schneider.25@gmail.com',
        'GIT_COMMITTER_NAME': 'Jim Schneider',
        'GIT_COMMITTER_EMAIL': 'jim.schneider.25@gmail.com',
        'GIT_DISCOVERY_ACROSS_FILESYSTEM': '1',
    })
    return subprocess.run(cmd, env=env, capture_output=True, text=True)

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    mode = sys.argv[1]

    if mode == '--status':
        sync_to_tmp()
        r = git(['git', 'status'])
        print(r.stdout)
        return

    if mode == '--diff':
        sync_to_tmp()
        r = git(['git', 'diff', 'HEAD'])
        print(r.stdout or "(nothing staged vs HEAD — use --status to see untracked)")
        return

    message = sys.argv[1]

    print("Syncing .git to /tmp...")
    sync_to_tmp()

    print("Staging all changes...")
    r = git(['git', 'add', '-A'])
    if r.returncode != 0 and 'embedded git' not in r.stderr:
        print(f"git add warning: {r.stderr.strip()}")

    print("Writing tree...")
    r = git(['git', 'write-tree'])
    tree_sha = r.stdout.strip()
    if not tree_sha:
        print(f"ERROR: write-tree failed: {r.stderr}")
        sys.exit(1)

    # Build commit-tree args (include parent if HEAD exists)
    ct_args = ['git', 'commit-tree', tree_sha, '-m', message]
    r_head = git(['git', 'rev-parse', '--verify', 'HEAD'])
    if r_head.returncode == 0:
        ct_args += ['-p', r_head.stdout.strip()]

    print("Creating commit...")
    r = git(ct_args)
    commit_sha = r.stdout.strip()
    if not commit_sha:
        print(f"ERROR: commit-tree failed: {r.stderr}")
        sys.exit(1)

    r = git(['git', 'update-ref', 'refs/heads/master', commit_sha])
    if r.returncode != 0:
        print(f"ERROR: update-ref failed: {r.stderr}")
        sys.exit(1)

    print("Syncing back to project...")
    sync_from_tmp()

    print(f"\n✓ Committed: {commit_sha[:12]}  {message}")
    r = git(['git', 'log', '--oneline', '-5'])
    print(r.stdout.strip())

if __name__ == '__main__':
    main()



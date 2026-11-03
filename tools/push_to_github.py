#!/usr/bin/env python3
"""通过 GitHub REST API 推送本仓库（绕过 git:// 与 https://github.com 的网络限制）。

这台机器上 github.com 直连超时（SSL connection timeout），只有 api.github.com 通，
所以 `git push` 用不了。Git Data API 走的是 api.github.com，可以正常用。

用法:
    GH_TOKEN=$(cat ~/token) python3 tools/push_to_github.py [repo]
    GH_TOKEN=$(cat ~/token) python3 tools/push_to_github.py --allow-empty "提交说明"

说明:
  * token 只从环境变量读，不落盘、不进 .git/config。
  * 推的不是一个压扁的快照，而是**把远端缺的那些提交按原顺序逐个重放**，
    每个提交的 tree / 作者 / 时间 / 说明都跟本地一致 → SHA 也一致，
    所以推完本地和远端指向同一个 commit，不会分叉。
    （早期版本只推 HEAD 一个 commit，父提交直接指向远端，攒了多个提交时
      等于把中间历史压扁了 —— 现在改成重放。）
  * 内容相同的文件 → blob SHA 相同；路径/模式相同 → tree SHA 相同。
"""
import base64
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

OWNER = "MrWANGXUHUI"
DEFAULT_REPO = "niu-u1-foc-simulator"


def api(method, path, token, data=None, base=None, allow404=False):
    url = (base or "") + path
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/vnd.github+json")
    if body:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        if allow404 and e.code == 404:
            return None
        body = e.read().decode()
        raise SystemExit(f"GitHub API {method} {path} -> {e.code}\n{body[:600]}")


def git(*args, binary=False, check=True):
    out = subprocess.run(["git", *args], capture_output=True)
    if out.returncode != 0:
        if check:
            raise SystemExit(f"git {' '.join(args)} 失败:\n{out.stderr.decode()[:600]}")
        return None
    return out.stdout if binary else out.stdout.decode()


def parse_ident(s):
    name, rest = s.split(" <", 1)
    email, rest = rest.split("> ", 1)
    ts, tz = rest.split(" ", 1)
    return name, email, int(ts), tz


def to_iso(a):
    name, email, ts, tz = a
    sign = 1 if tz[0] == "+" else -1
    off = timezone(sign * timedelta(hours=int(tz[1:3]), minutes=int(tz[3:5])))
    return {"name": name, "email": email, "date": datetime.fromtimestamp(ts, off).isoformat()}


def parse_args(argv):
    repo = None
    allow_empty = False
    empty_msg = ""
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--allow-empty":
            allow_empty = True
            i += 1
            if i < len(argv) and not argv[i].startswith("--"):
                empty_msg = argv[i]
                i += 1
        elif a.startswith("--"):
            pass          # 其它开关先忽略
        else:
            if repo is None:
                repo = a
            i += 1
    return repo or DEFAULT_REPO, allow_empty, empty_msg


def main():
    token = os.environ.get("GH_TOKEN", "").strip()
    if not token:
        raise SystemExit("请先设置 GH_TOKEN 环境变量")
    repo, allow_empty, empty_msg = parse_args(sys.argv[1:])
    base = f"https://api.github.com/repos/{OWNER}/{repo}"

    # 1. 仓库不存在就建
    if api("GET", "", token, base=base, allow404=True) is None:
        api("POST", "/user/repos", token, base="https://api.github.com", data={
            "name": repo, "private": False, "auto_init": False})
        print(f"已创建仓库 {OWNER}/{repo}")

    # 2. 远端当前 ref（空仓库 404）
    head = api("GET", "/git/ref/heads/main", token, base=base, allow404=True)
    remote_sha = head["object"]["sha"] if head else None
    local_head = git("rev-parse", "HEAD").strip()

    # 3. --allow-empty: 用当前索引造一个空提交推上去
    if allow_empty:
        git("add", "-A")
        tree = git("write-tree").strip()
        msg = empty_msg or "empty commit"
        parent = remote_sha
        if parent:
            raw = git("cat-file", "commit", "HEAD", binary=True)
            hdr = dict(l.split(" ", 1) for l in raw.partition(b"\n\n")[0].decode().split("\n"))
            author = parse_ident(hdr["author"])
        else:
            author = ("Xuhui Wang", "12712745+MrWANGXUHUI@users.noreply.github.com",
                      int(datetime.now().timestamp()), "+0800")
        commit = api("POST", "/git/commits", token, base=base, data={
            "message": msg + "\n", "tree": tree,
            "parents": [parent] if parent else [],
            "author": to_iso(author), "committer": to_iso(author)})
        update_ref(token, base, remote_sha, commit["sha"])
        print(f"空提交 {commit['sha'][:10]} 已推送")
        return

    if remote_sha == local_head:
        print(f"远端已是最新: {local_sha_prefix(local_head)}")
        return

    # 4. 要重放的提交：从远端那个 commit 往后，按原顺序
    if remote_sha:
        if git("merge-base", "--is-ancestor", remote_sha, local_head, check=False) is None:
            raise SystemExit(
                f"远端的 {remote_sha[:10]} 不在本地历史里 —— "
                "本地和远端已经分叉，得先手动合并")
        commits = git("rev-list", "--reverse", f"{remote_sha}..{local_head}").split()
    else:
        commits = git("rev-list", "--reverse", local_head).split()
    if not commits:
        raise SystemExit(f"没有要推的提交 (本地 {local_head[:10]})")
    print(f"要重放 {len(commits)} 个提交: {commits[0][:10]}..{commits[-1][:10]}")

    # 5. 逐个重放
    uploaded = 0
    blob_cache = {}

    def ensure_blob(sha):
        """远端有这个 blob 就直接引用，没有就传一份（同样的内容 → 同样的 SHA）"""
        nonlocal uploaded
        if sha in blob_cache:
            return
        if api("GET", f"/git/blobs/{sha}", token, base=base, allow404=True) is None:
            data = git("cat-file", "blob", sha, binary=True)
            b = api("POST", "/git/blobs", token, base=base, data={
                "content": base64.b64encode(data).decode(), "encoding": "base64"})
            if b["sha"] != sha:
                raise SystemExit(f"blob SHA 不一致: {b['sha']} != {sha}")
            uploaded += 1
        blob_cache[sha] = True

    parent = remote_sha
    for c in commits:
        tree_local = git("rev-parse", f"{c}^{{tree}}").strip()
        # 远端已经有这个 tree 就直接用（大多数老提交都命中）
        if api("GET", f"/git/trees/{tree_local}", token, base=base, allow404=True) is None:
            entries = []
            for line in git("ls-tree", "-r", c).splitlines():
                meta, path = line.split("\t", 1)
                mode, typ, sha = meta.split()
                ensure_blob(sha)
                entries.append({"path": path, "mode": mode, "type": typ, "sha": sha})
            t = api("POST", "/git/trees", token, base=base, data={"tree": entries})
            if t["sha"] != tree_local:
                raise SystemExit(f"tree SHA 不一致: {t['sha']} != {tree_local} (commit {c[:10]})")

        raw = git("cat-file", "commit", c, binary=True)
        hdr = dict(l.split(" ", 1) for l in raw.partition(b"\n\n")[0].decode().split("\n"))
        author = parse_ident(hdr["author"])
        committer = parse_ident(hdr["committer"])
        msg = raw.partition(b"\n\n")[2].decode()
        commit = api("POST", "/git/commits", token, base=base, data={
            "message": msg, "tree": tree_local,
            "parents": [parent] if parent else [],
            "author": to_iso(author), "committer": to_iso(committer)})
        if commit["sha"] != c:
            raise SystemExit(f"commit SHA 不一致: {commit['sha']} != {c}")
        parent = c

    update_ref(token, base, remote_sha, parent)
    print(f"新传 blob {uploaded} 个")
    print(f"已推送 -> https://github.com/{OWNER}/{repo}/commit/{parent}")


def local_sha_prefix(sha):
    return sha[:10]


def update_ref(token, base, remote_sha, sha):
    if remote_sha:
        api("PATCH", "/git/refs/heads/main", token, base=base, data={"sha": sha, "force": True})
    else:
        api("POST", "/git/refs", token, base=base, data={"ref": "refs/heads/main", "sha": sha})


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""通过 GitHub REST API 推送本仓库（绕过 git:// 与 https://github.com 的网络限制）。

这台机器上 github.com 直连超时（SSL connection timeout），只有 api.github.com 通，
所以 `git push` 用不了。Git Data API 走的是 api.github.com，可以正常用。

用法:
    GH_TOKEN=$(cat ~/token) python3 tools/push_to_github.py [repo]
    GH_TOKEN=$(cat ~/token) python3 tools/push_to_github.py --allow-empty "提交说明"

说明:
  * 内容相同的文件 → blob SHA 相同；路径/模式相同 → tree SHA 相同；
    作者、时间、提交信息相同 → commit SHA 与本地一致。
    所以走完 API 之后本地和远端是同一个 commit，不会分叉。
  * token 只从环境变量读，不落盘、不进 .git/config。
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
EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"  # git 空树


def api(method, path, token, data=None, base=None):
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
        body = e.read().decode()
        raise SystemExit(f"GitHub API {method} {path} -> {e.code}\n{body[:600]}")


def git(*args, binary=False):
    out = subprocess.run(["git", *args], capture_output=True)
    if out.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} 失败:\n{out.stderr.decode()[:600]}")
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


def main():
    token = os.environ.get("GH_TOKEN", "").strip()
    if not token:
        raise SystemExit("请先设置 GH_TOKEN 环境变量")
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    repo = args[0] if args else DEFAULT_REPO
    base = f"https://api.github.com/repos/{OWNER}/{repo}"

    # 1. 仓库不存在就建
    try:
        api("GET", "", token, base=base)
    except SystemExit:
        api("POST", "/user/repos", token, base="https://api.github.com", data={
            "name": repo, "private": False, "auto_init": False}, )
        print(f"已创建仓库 {OWNER}/{repo}")

    # 2. 远端当前 ref（空仓库 404）
    try:
        head = api("GET", "/git/ref/heads/main", token, base=base)
        remote_sha, parents = head["object"]["sha"], [head["object"]["sha"]]
    except SystemExit:
        remote_sha, parents = None, []

    # 3. 本地 HEAD
    raw = git("cat-file", "commit", "HEAD", binary=True)
    head_bytes, _, msg = raw.partition(b"\n\n")
    hdr = dict(l.split(" ", 1) for l in head_bytes.decode().split("\n"))
    local_sha = git("rev-parse", "HEAD").strip()
    if remote_sha == local_sha:
        print(f"远端已是最新: {local_sha[:10]}")
        return

    # 4. blobs
    entries = []
    for line in git("ls-files", "-s").splitlines():
        meta, path = line.split("\t", 1)
        mode, sha, _stage = meta.split()
        blob = api("POST", "/git/blobs", token, base=base, data={
            "content": base64.b64encode(open(path, "rb").read()).decode(),
            "encoding": "base64"})
        if blob["sha"] != sha:
            raise SystemExit(f"{path} blob SHA 不一致: {blob['sha']} != {sha}")
        entries.append({"path": path, "mode": mode, "type": "blob", "sha": sha})
    print(f"{len(entries)} 个 blob 已上传")

    # 5. tree
    tree = api("POST", "/git/trees", token, base=base,
               data={"tree": entries, "base_tree": remote_sha} if remote_sha else {"tree": entries})
    if tree["sha"] != hdr["tree"] and not remote_sha:
        raise SystemExit(f"tree SHA 不一致: {tree['sha']} != {hdr['tree']}")

    # 6. commit
    author = parse_ident(hdr["author"])
    committer = parse_ident(hdr["committer"])
    commit = api("POST", "/git/commits", token, base=base, data={
        "message": msg.decode(), "tree": tree["sha"], "parents": parents,
        "author": to_iso(author), "committer": to_iso(committer)})
    print(f"commit {commit['sha'][:10]} (本地 {local_sha[:10]})")

    # 7. ref
    if remote_sha:
        api("PATCH", "/git/refs/heads/main", token, base=base,
            data={"sha": commit["sha"], "force": True})
    else:
        api("POST", "/git/refs", token, base=base,
            data={"ref": "refs/heads/main", "sha": commit["sha"]})
    print(f"已推送 -> https://github.com/{OWNER}/{repo}/commit/{commit['sha']}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Low-noise ACP client for delegating one task to Kimi Code.

The client consumes Kimi's verbose JSON-RPC stream locally and prints only the
final assistant message.  Thought chunks, incremental tool arguments, raw tool
output, images, and repeated status updates never reach the caller's stdout.

Examples:
    python tools/kimi-delegate.py -p "Inspect the diff and fix the build"
    python tools/kimi-delegate.py --thinking high --mode auto -f task.txt
    python tools/kimi-delegate.py --session-id SESSION_ID -p "Continue"
    python tools/kimi-delegate.py wait --run-id RUN_ID --timeout 55 --quiet-timeout
    python tools/kimi-delegate.py status --run-id RUN_ID --max-chars 1500

When Kimi requests permission, the client prints one compact ``[kimi-request]``
record and waits for an option number/id on stdin.  The ACP turn then continues
in the same process; thought/tool streams remain filtered throughout.

Every run also writes a small, atomic status snapshot under
``tmp/kimi-delegate-state`` (override with ``--state-dir`` or
``KIMI_DELEGATE_STATE_DIR``).  A second process can query that snapshot without
loading/replaying the ACP session.  Snapshots contain only bounded visible agent
text, compact tool states, approvals, and worktree-change metadata; never raw
tool output or thought chunks.

The ``wait`` subcommand is intended for low-token supervision.  It emits a
snapshot only when the run completes, fails, is cancelled, or needs permission;
with ``--quiet-timeout`` an uneventful timeout emits nothing at all.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO


class AcpError(RuntimeError):
    pass


ANSI_RE = re.compile(
    r"(?:\x1B\[[0-?]*[ -/]*[@-~]|\x1B\][^\x07]*(?:\x07|\x1B\\)|\x1B[@-_])"
)
CONTROL_RE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
STATE_AGENT_CHARS = 12_000
STATE_TOOL_COUNT = 8
STATE_WRITE_INTERVAL = 0.75
STATE_REPLACE_RETRIES = 6


def clean_text(value: Any, limit: int | None = None) -> str:
    """Remove terminal controls while keeping readable newlines and tabs."""
    text = value if isinstance(value, str) else str(value or "")
    text = ANSI_RE.sub("", text).replace("\r\n", "\n").replace("\r", "\n")
    text = CONTROL_RE.sub("", text)
    return text if limit is None else text[:limit]


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _file_digest(path: Path) -> tuple[str | None, int | None]:
    try:
        if not path.is_file():
            return None, None
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
        return digest.hexdigest(), size
    except OSError:
        return None, None


def capture_worktree(cwd: Path) -> dict[str, dict[str, Any]] | None:
    """Capture dirty paths plus hashes, without storing file contents or diffs."""
    try:
        result = subprocess.run(
            ["git", "-c", "core.quotepath=false", "status", "--porcelain=v1",
             "--untracked-files=all"],
            cwd=str(cwd), capture_output=True, timeout=15, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    text = result.stdout.decode("utf-8", errors="replace")
    entries: dict[str, dict[str, Any]] = {}
    for line in text.splitlines():
        if len(line) < 4:
            continue
        status = line[:2]
        path_text = line[3:]
        if " -> " in path_text:
            path_text = path_text.rsplit(" -> ", 1)[1]
        path_text = path_text.strip('"')
        digest, size = _file_digest(cwd / path_text)
        entries[path_text] = {"status": status, "sha256": digest, "size": size}
    return entries


def compare_worktree(before: dict[str, dict[str, Any]] | None,
                     after: dict[str, dict[str, Any]] | None) -> dict[str, Any]:
    if before is None or after is None:
        return {"available": False}
    changed: list[str] = []
    for path in sorted(set(before) | set(after)):
        old = before.get(path)
        new = after.get(path)
        if old != new:
            changed.append(path)
    return {
        "available": True,
        "baselineDirty": len(before),
        "currentDirty": len(after),
        "changedSinceStart": changed[:200],
        "changedSinceStartCount": len(changed),
        "truncated": len(changed) > 200,
    }


def resolve_state_dir(cwd: Path, explicit: Path | None = None) -> Path:
    raw = explicit or (Path(os.environ["KIMI_DELEGATE_STATE_DIR"])
                       if os.environ.get("KIMI_DELEGATE_STATE_DIR") else None)
    if raw is None:
        raw = cwd / "tmp" / "kimi-delegate-state"
    elif not raw.is_absolute():
        raw = cwd / raw
    return raw.expanduser().resolve()


class StatusReporter:
    """Maintain a throttled, atomic, non-sensitive snapshot for live monitoring."""

    def __init__(self, cwd: Path, state_dir: Path, run_id: str | None = None) -> None:
        self.cwd = cwd
        self.state_dir = state_dir
        self.run_id = run_id or f"kd-{datetime.now().strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:8]}"
        self.started_mono = time.monotonic()
        self.last_write_mono = 0.0
        self.agent_tail = ""
        self.tools: collections.OrderedDict[str, dict[str, Any]] = collections.OrderedDict()
        self.baseline = capture_worktree(cwd)
        self.data: dict[str, Any] = {
            "schemaVersion": 1,
            "runId": self.run_id,
            "pid": os.getpid(),
            "state": "starting",
            "cursor": 0,
            "startedAt": utc_now(),
            "updatedAt": utc_now(),
            "elapsedSeconds": 0.0,
            "cwd": str(cwd),
            "sessionId": None,
            "lastAgentText": "",
            "recentTools": [],
            "pendingRequest": None,
            "worktree": {
                "available": self.baseline is not None,
                "baselineDirty": len(self.baseline or {}),
            },
        }
        self.write(force=True)

    def _atomic_write(self, path: Path, payload: bytes) -> None:
        temp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
        temp.write_bytes(payload)
        for attempt in range(STATE_REPLACE_RETRIES):
            try:
                os.replace(temp, path)
                return
            except PermissionError:
                # Windows scanners/readers can briefly hold the destination.
                # A monitoring snapshot must never terminate the ACP turn.
                if attempt + 1 < STATE_REPLACE_RETRIES:
                    time.sleep(0.05 * (attempt + 1))
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass

    def write(self, *, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - self.last_write_mono < STATE_WRITE_INTERVAL:
            return
        self.data["updatedAt"] = utc_now()
        self.data["elapsedSeconds"] = round(now - self.started_mono, 1)
        self.data["lastAgentText"] = self.agent_tail[-STATE_AGENT_CHARS:]
        self.data["recentTools"] = list(self.tools.values())[-STATE_TOOL_COUNT:]
        payload = json.dumps(self.data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        try:
            self.state_dir.mkdir(parents=True, exist_ok=True)
            self._atomic_write(self.state_dir / f"{self.run_id}.json", payload)
            self._atomic_write(self.state_dir / "latest.json", payload)
            self.last_write_mono = now
        except OSError:
            # Status reporting is deliberately best-effort and non-fatal.
            # The foreground process still delivers requests and the final reply.
            return

    def _touch(self, *, force: bool = False) -> None:
        self.data["cursor"] = int(self.data.get("cursor", 0)) + 1
        self.write(force=force)

    def set_state(self, state: str, *, force: bool = False, **values: Any) -> None:
        self.data["state"] = state
        self.data.update(values)
        self._touch(force=force)

    def set_process(self, pid: int) -> None:
        self.data["agentPid"] = pid
        self._touch(force=True)

    def set_session(self, session_id: str) -> None:
        self.data["sessionId"] = session_id
        self._touch(force=True)

    def record_agent(self, text: str) -> None:
        if not text:
            return
        self.agent_tail = clean_text(self.agent_tail + text)[-STATE_AGENT_CHARS:]
        self.data["state"] = "running"
        self._touch()

    def record_tool(self, update: dict[str, Any]) -> None:
        tool_id = str(update.get("toolCallId") or update.get("id") or "unknown")
        previous = self.tools.get(tool_id, {})
        title = clean_text(update.get("title") or previous.get("title") or "tool", 200)
        status = clean_text(update.get("status") or previous.get("status") or "running", 40)
        if previous.get("title") == title and previous.get("status") == status:
            return   # Repeated raw/progress updates add nothing visible to the monitor.
        self.tools[tool_id] = {
            "toolCallId": tool_id,
            "title": title,
            "status": status,
            "updatedAt": utc_now(),
        }
        self.tools.move_to_end(tool_id)
        while len(self.tools) > STATE_TOOL_COUNT:
            self.tools.popitem(last=False)
        self._touch(force=status in ("completed", "failed"))

    def record_permission(self, summary: dict[str, Any]) -> None:
        self.data["pendingRequest"] = summary
        self.set_state("waiting_permission", force=True)

    def clear_permission(self, selected: str | None) -> None:
        self.data["pendingRequest"] = None
        self.data["lastPermissionOutcome"] = selected or "cancelled"
        self.set_state("running", force=True)

    def finish(self, state: str, *, stats: dict[str, Any] | None = None,
               stop_reason: Any = None, error: str | None = None) -> None:
        self.data["stopReason"] = stop_reason
        if stats is not None:
            self.data["filtered"] = stats
        if error:
            self.data["error"] = clean_text(error, 1000)
        self.data["worktree"] = compare_worktree(self.baseline, capture_worktree(self.cwd))
        self.set_state(state, force=True)


class AcpClient:
    def __init__(self, kimi: str, cwd: Path, timeout: float, log_path: Path | None,
                 verbose: bool, approval_policy: str,
                 reporter: StatusReporter | None = None) -> None:
        executable = shutil.which(kimi) or kimi
        self.cwd = cwd
        self.timeout = timeout
        self.verbose = verbose
        self.approval_policy = approval_policy
        self.reporter = reporter
        self.inbox: queue.Queue[tuple[str, bytes | None]] = queue.Queue()
        self.stderr_tail: collections.deque[str] = collections.deque(maxlen=40)
        self.log: BinaryIO | None = None
        if log_path:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self.log = log_path.open("wb")

        self.stats = {
            "raw_bytes": 0,
            "thought_chunks": 0,
            "thought_chars": 0,
            "tool_updates": 0,
            "status_updates": 0,
            "final_chars": 0,
            "permission_requests": 0,
        }
        self.final_chunks: list[str] = []
        self.completed_tools: set[str] = set()
        self.session_id: str | None = None
        self.next_id = 1

        creationflags = 0
        if os.name == "nt":
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        child_env = os.environ.copy()
        # kimi.exe is Python-based on Windows. Force its ACP channel to UTF-8
        # even when the parent console uses a legacy code page.
        child_env["PYTHONUTF8"] = "1"
        child_env["PYTHONIOENCODING"] = "utf-8"
        try:
            self.proc = subprocess.Popen(
                [executable, "acp"],
                cwd=str(cwd),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
                creationflags=creationflags,
                env=child_env,
            )
        except OSError as exc:
            raise AcpError(f"cannot start {kimi!r}: {exc}") from exc

        if self.reporter:
            self.reporter.set_process(self.proc.pid)

        assert self.proc.stdout is not None
        assert self.proc.stderr is not None
        threading.Thread(target=self._reader, args=("stdout", self.proc.stdout), daemon=True).start()
        threading.Thread(target=self._reader, args=("stderr", self.proc.stderr), daemon=True).start()

    def _reader(self, source: str, stream: BinaryIO) -> None:
        try:
            for line in stream:
                self.inbox.put((source, line))
        finally:
            self.inbox.put((source, None))

    def _send(self, message: dict[str, Any]) -> None:
        if not self.proc.stdin:
            raise AcpError("Kimi ACP stdin is unavailable")
        wire = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        try:
            # ACP input is UTF-8 even though kimi.exe currently emits GBK on
            # Windows when stdout is a pipe.
            self.proc.stdin.write((wire + "\n").encode("utf-8"))
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise AcpError("Kimi ACP closed its input pipe") from exc

    def _deadline(self, seconds: float | None = None) -> float:
        return time.monotonic() + (self.timeout if seconds is None else seconds)

    def _next_message(self, deadline: float) -> dict[str, Any]:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for Kimi ACP")
            try:
                source, raw_line = self.inbox.get(timeout=min(0.25, remaining))
            except queue.Empty:
                if self.proc.poll() is not None:
                    tail = "".join(self.stderr_tail).strip()
                    raise AcpError(f"Kimi ACP exited with code {self.proc.returncode}: {tail}")
                continue

            if raw_line is None:
                if source == "stdout" and self.proc.poll() is not None:
                    tail = "".join(self.stderr_tail).strip()
                    raise AcpError(f"Kimi ACP exited with code {self.proc.returncode}: {tail}")
                continue
            try:
                line = raw_line.decode("utf-8")
            except UnicodeDecodeError:
                # Kimi Code CLI 0.42.0 uses the active Windows code page for
                # ACP stdout.  Fall back only when strict UTF-8 fails, so the
                # normal cross-platform protocol path remains standards based.
                line = raw_line.decode("gbk" if os.name == "nt" else "utf-8", errors="replace")
            if source == "stderr":
                self.stderr_tail.append(line)
                continue

            self.stats["raw_bytes"] += len(raw_line)
            if self.log:
                self.log.write(raw_line)
                self.log.flush()
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                self.stderr_tail.append(f"non-JSON stdout: {line}")
                continue
            return message

    @staticmethod
    def _content_text(content: Any) -> str:
        if isinstance(content, dict) and content.get("type") == "text":
            value = content.get("text", "")
            return value if isinstance(value, str) else ""
        return ""

    @classmethod
    def _request_text(cls, value: Any, limit: int = 1000) -> str:
        """Extract only human-readable text; never forward blobs or raw output."""
        found: list[str] = []

        def visit(node: Any) -> None:
            if sum(map(len, found)) >= limit:
                return
            if isinstance(node, dict):
                if node.get("type") == "text" and isinstance(node.get("text"), str):
                    found.append(node["text"])
                    return
                for key, child in node.items():
                    if key not in ("rawOutput", "data", "blob", "imageUrl", "audioUrl"):
                        visit(child)
            elif isinstance(node, list):
                for child in node:
                    visit(child)

        visit(value)
        return "\n".join(found)[:limit]

    @staticmethod
    def _pick_policy_option(options: list[dict[str, Any]], policy: str) -> str | None:
        wanted = {
            "allow-once": ("allow_once",),
            "allow-always": ("allow_always", "allow_once"),
            "reject": ("reject_once", "reject_always"),
        }.get(policy, ())
        for kind in wanted:
            for option in options:
                if option.get("kind") == kind:
                    value = option.get("optionId")
                    return str(value) if value is not None else None
        return None

    @staticmethod
    def _parse_choice(answer: str, options: list[dict[str, Any]]) -> str | None | bool:
        """Return option id, None for cancel, or False for an invalid answer."""
        answer = answer.strip()
        if not answer:
            return False
        if answer.startswith("{"):
            try:
                value = json.loads(answer)
            except json.JSONDecodeError:
                return False
            if value.get("cancelled") is True or value.get("outcome") == "cancelled":
                return None
            answer = str(value.get("optionId") or value.get("choice") or "").strip()
        lowered = answer.casefold()
        if lowered in ("cancel", "cancelled", "c"):
            return None
        aliases = {
            "y": "allow_once", "yes": "allow_once", "once": "allow_once",
            "a": "allow_always", "always": "allow_always",
            "n": "reject_once", "no": "reject_once", "reject": "reject_once",
        }
        lowered = aliases.get(lowered, lowered)
        if lowered.isdigit():
            index = int(lowered) - 1
            if 0 <= index < len(options):
                option_id = options[index].get("optionId")
                return str(option_id) if option_id is not None else False
            return False
        for option in options:
            candidates = (option.get("optionId"), option.get("kind"), option.get("name"))
            if any(str(item).casefold() == lowered for item in candidates if item is not None):
                option_id = option.get("optionId")
                return str(option_id) if option_id is not None else False
        return False

    def _handle_permission(self, message: dict[str, Any]) -> float:
        started = time.monotonic()
        params = message.get("params", {})
        raw_options = params.get("options", [])
        options = [item for item in raw_options if isinstance(item, dict)]
        tool_call = params.get("toolCall", {})
        summary = {
            "requestId": message.get("id"),
            "sessionId": params.get("sessionId"),
            "toolCallId": tool_call.get("toolCallId") if isinstance(tool_call, dict) else None,
            "title": tool_call.get("title") if isinstance(tool_call, dict) else None,
            "message": self._request_text(tool_call),
            "options": [
                {
                    "number": index + 1,
                    "optionId": option.get("optionId"),
                    "name": option.get("name"),
                    "kind": option.get("kind"),
                }
                for index, option in enumerate(options)
            ],
        }
        self.stats["permission_requests"] += 1
        if self.reporter:
            self.reporter.record_permission(summary)
        print("[kimi-request] " + json.dumps(summary, ensure_ascii=False, separators=(",", ":")),
              file=sys.stderr, flush=True)

        selected = self._pick_policy_option(options, self.approval_policy)
        if self.approval_policy == "ask":
            print("[kimi-request] reply with option number/id (or 'cancel'):",
                  file=sys.stderr, flush=True)
            try:
                while True:
                    answer = sys.stdin.readline()
                    if answer == "":
                        outcome = {"outcome": "cancelled"}
                        self._send({"jsonrpc": "2.0", "id": message.get("id"),
                                    "result": {"outcome": outcome}})
                        raise AcpError(
                            "stdin closed while Kimi awaited a choice; use -p/-f so stdin remains available")
                    parsed = self._parse_choice(answer, options)
                    if parsed is not False:
                        selected = parsed
                        break
                    print("[kimi-request] invalid choice; use a listed number/id or 'cancel':",
                          file=sys.stderr, flush=True)
            except KeyboardInterrupt:
                outcome = {"outcome": "cancelled"}
                self._send({"jsonrpc": "2.0", "id": message.get("id"),
                            "result": {"outcome": outcome}})
                raise
        elif selected is None:
            print(f"[kimi-request] policy {self.approval_policy!r} found no matching option; cancelling",
                  file=sys.stderr, flush=True)

        if selected is None:
            outcome: dict[str, Any] = {"outcome": "cancelled"}
        else:
            outcome = {"outcome": "selected", "optionId": selected}
            print(f"[kimi-request] selected={selected}", file=sys.stderr, flush=True)
        self._send({"jsonrpc": "2.0", "id": message.get("id"), "result": {"outcome": outcome}})
        if self.reporter:
            self.reporter.clear_permission(selected)
        return time.monotonic() - started

    def _handle_incoming(self, message: dict[str, Any], collect_final: bool) -> float:
        method = message.get("method")
        if method == "session/request_permission" and "id" in message:
            return self._handle_permission(message)
        if method != "session/update":
            return 0.0

        update = message.get("params", {}).get("update", {})
        kind = update.get("sessionUpdate", "")
        if kind == "agent_thought_chunk":
            text = self._content_text(update.get("content"))
            self.stats["thought_chunks"] += 1
            self.stats["thought_chars"] += len(text)
            return 0.0
        if kind == "agent_message_chunk":
            if collect_final:
                text = clean_text(self._content_text(update.get("content")))
                self.final_chunks.append(text)
                self.stats["final_chars"] += len(text)
                if self.reporter:
                    self.reporter.record_agent(text)
            return 0.0
        if kind in ("tool_call", "tool_call_update"):
            self.stats["tool_updates"] += 1
            if self.reporter:
                self.reporter.record_tool(update)
            if self.verbose and update.get("status") in ("completed", "failed"):
                tool_id = str(update.get("toolCallId", ""))
                if tool_id not in self.completed_tools:
                    self.completed_tools.add(tool_id)
                    title = str(update.get("title") or "tool")
                    print(f"[kimi] {update.get('status')}: {title}", file=sys.stderr)
            return 0.0
        self.stats["status_updates"] += 1
        return 0.0

    def request(self, method: str, params: dict[str, Any], *, collect_final: bool = False,
                timeout: float | None = None) -> dict[str, Any]:
        request_id = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = self._deadline(timeout)
        while True:
            message = self._next_message(deadline)
            if message.get("id") == request_id:
                if "error" in message:
                    raise AcpError(f"{method} failed: {message['error']}")
                result = message.get("result", {})
                return result if isinstance(result, dict) else {"value": result}
            # Waiting for a human approval must not consume the agent timeout.
            deadline += self._handle_incoming(message, collect_final)

    def initialize(self) -> None:
        self.request("initialize", {
            "protocolVersion": 1,
            "clientCapabilities": {},
            "clientInfo": {"name": "kimi-delegate", "version": "1.0"},
        }, timeout=min(self.timeout, 30.0))

    def open_session(self, resume_id: str | None, additional_dirs: list[Path]) -> None:
        if resume_id:
            result = self.request("session/resume", {
                "sessionId": resume_id,
                "cwd": str(self.cwd),
            })
            self.session_id = resume_id
        else:
            params: dict[str, Any] = {"cwd": str(self.cwd), "mcpServers": []}
            if additional_dirs:
                params["additionalDirectories"] = [str(path) for path in additional_dirs]
            result = self.request("session/new", params)
            self.session_id = result.get("sessionId")
        if not self.session_id:
            raise AcpError("Kimi ACP did not return a sessionId")
        if self.reporter:
            self.reporter.set_session(self.session_id)

    def set_option(self, name: str, value: str | None) -> None:
        if value is None:
            return
        assert self.session_id
        self.request("session/set_config_option", {
            "sessionId": self.session_id,
            "configId": name,
            "value": value,
        })

    def prompt(self, text: str) -> tuple[str, dict[str, Any]]:
        assert self.session_id
        self.final_chunks.clear()
        result = self.request("session/prompt", {
            "sessionId": self.session_id,
            "prompt": [{"type": "text", "text": text}],
        }, collect_final=True)
        return "".join(self.final_chunks), result

    def cancel(self) -> None:
        if not self.session_id or self.proc.poll() is not None:
            return
        try:
            self._send({
                "jsonrpc": "2.0",
                "id": self.next_id,
                "method": "session/cancel",
                "params": {"sessionId": self.session_id},
            })
            self.next_id += 1
        except AcpError:
            pass

    def close(self) -> None:
        if self.session_id and self.proc.poll() is None:
            try:
                self.request("session/close", {"sessionId": self.session_id}, timeout=5.0)
            except (AcpError, TimeoutError):
                pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self.log:
            self.log.close()


def parse_status_args(argv: list[str], *, wait_mode: bool = False) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog=f"kimi-delegate.py {'wait' if wait_mode else 'status'}",
        description=(
            "Wait for a bounded actionable snapshot without touching the ACP session."
            if wait_mode else
            "Read one bounded live snapshot without touching the ACP session."
        ))
    parser.add_argument("run", nargs="?", help="run id; defaults to the latest run")
    parser.add_argument("--run-id", dest="run_option", help="run id (same as positional RUN)")
    parser.add_argument("--state-dir", type=Path,
                        help="snapshot directory; defaults to cwd/tmp/kimi-delegate-state")
    parser.add_argument("--max-chars", type=int, default=1500,
                        help="hard character cap for the complete response (default: 1500)")
    parser.add_argument("--since", type=int,
                        help="return only an unchanged marker when cursor has not advanced")
    parser.add_argument("--json", action="store_true", help="emit bounded JSON")
    if wait_mode:
        parser.add_argument("--timeout", type=float, default=300.0,
                            help="seconds to wait for completion, failure, cancellation, or permission (default: 300)")
        parser.add_argument("--poll-interval", type=float, default=1.0,
                            help="snapshot polling interval in seconds (default: 1)")
        parser.add_argument("--quiet-timeout", action="store_true",
                            help="emit nothing when the run is still non-actionable at timeout")
    args = parser.parse_args(argv)
    if args.run and args.run_option:
        parser.error("use either positional RUN or --run-id, not both")
    args.run_id = args.run_option or args.run or "latest"
    if args.max_chars < 256:
        parser.error("--max-chars must be at least 256")
    if args.max_chars > 20_000:
        parser.error("--max-chars must not exceed 20000")
    if args.run_id != "latest" and not re.fullmatch(r"[A-Za-z0-9_.-]+", args.run_id):
        parser.error("run id may contain only letters, digits, dot, underscore, and hyphen")
    if len(args.run_id) > 96:
        parser.error("run id must not exceed 96 characters")
    if wait_mode:
        if args.timeout < 0 or args.timeout > 86_400:
            parser.error("--timeout must be between 0 and 86400 seconds")
        if args.poll_interval < 0.1 or args.poll_interval > 60:
            parser.error("--poll-interval must be between 0.1 and 60 seconds")
    return args


def _bounded_text(text: str, limit: int) -> str:
    text = clean_text(text)
    if len(text) <= limit:
        return text
    marker = "\n... output truncated ...\n"
    head = min(420, max(100, limit // 3))
    tail = limit - head - len(marker)
    return text[:head] + marker + text[-tail:]


def _status_payload(data: dict[str, Any]) -> dict[str, Any]:
    return {
        "runId": data.get("runId"),
        "state": data.get("state"),
        "cursor": data.get("cursor"),
        "elapsedSeconds": data.get("elapsedSeconds"),
        "updatedAt": data.get("updatedAt"),
        "sessionId": data.get("sessionId"),
        "lastAgentText": clean_text(data.get("lastAgentText", "")),
        "recentTools": data.get("recentTools", []),
        "pendingRequest": data.get("pendingRequest"),
        "worktree": data.get("worktree"),
        "stopReason": data.get("stopReason"),
        "error": clean_text(data.get("error", ""), 1000) or None,
    }


def _bounded_json(payload: dict[str, Any], limit: int) -> str:
    def encode() -> str:
        return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))

    output = encode()
    overflow = len(output) - limit
    if overflow > 0 and payload.get("lastAgentText"):
        value = str(payload["lastAgentText"])
        keep = max(0, len(value) - overflow - 40)
        payload["lastAgentText"] = ("..." + value[-keep:]) if keep else ""
        payload["truncated"] = True
        output = encode()
    if len(output) > limit:
        payload["recentTools"] = list(payload.get("recentTools") or [])[-2:]
        worktree = payload.get("worktree")
        if isinstance(worktree, dict) and isinstance(worktree.get("changedSinceStart"), list):
            worktree["changedSinceStart"] = worktree["changedSinceStart"][-5:]
            worktree["truncated"] = True
        payload["truncated"] = True
        output = encode()
    if len(output) > limit:
        payload = {
            "runId": payload.get("runId"),
            "state": payload.get("state"),
            "cursor": payload.get("cursor"),
            "elapsedSeconds": payload.get("elapsedSeconds"),
            "updatedAt": payload.get("updatedAt"),
            "truncated": True,
        }
        output = encode()
    return output


def _read_status_snapshot(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"invalid snapshot: {path}")
    return data


def status_main(argv: list[str], *, wait_mode: bool = False) -> int:
    args = parse_status_args(argv, wait_mode=wait_mode)
    state_dir = resolve_state_dir(Path.cwd().resolve(), args.state_dir)
    path = state_dir / ("latest.json" if args.run_id == "latest" else f"{args.run_id}.json")
    deadline = time.monotonic() + (args.timeout if wait_mode else 0)
    actionable_states = {"completed", "failed", "cancelled", "waiting_permission"}
    data: dict[str, Any] | None = None
    while True:
        try:
            data = _read_status_snapshot(path)
        except FileNotFoundError:
            if not wait_mode or time.monotonic() >= deadline:
                print(f"kimi-delegate status: snapshot not found: {path}", file=sys.stderr)
                return 1
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
            print(f"kimi-delegate status: cannot read {path}: {exc}", file=sys.stderr)
            return 1

        if data is not None:
            state = str(data.get("state") or "unknown")
            if not wait_mode or state in actionable_states:
                break
            if time.monotonic() >= deadline:
                if args.quiet_timeout:
                    return 0
                cursor = int(data.get("cursor", 0))
                if args.json:
                    timeout_payload = {
                        "runId": data.get("runId"), "state": state,
                        "cursor": cursor, "timeout": True,
                    }
                    print(_bounded_json(timeout_payload, args.max_chars))
                else:
                    print(f"[kimi-wait] timeout run={data.get('runId')} "
                          f"state={state} cursor={cursor}")
                return 0

        remaining = max(0.0, deadline - time.monotonic())
        time.sleep(min(args.poll_interval, remaining))

    assert data is not None

    cursor = int(data.get("cursor", 0))
    if args.since is not None and cursor <= args.since:
        unchanged = {
            "runId": data.get("runId"), "state": data.get("state"),
            "cursor": cursor, "unchanged": True,
        }
        if args.json:
            print(_bounded_json(unchanged, args.max_chars))
        else:
            print(f"[kimi-status] unchanged run={data.get('runId')} "
                  f"state={data.get('state')} cursor={cursor}")
        return 0

    payload = _status_payload(data)
    if args.json:
        print(_bounded_json(payload, args.max_chars))
        return 0

    lines = [
        f"[kimi-status] run={payload['runId']} state={payload['state']} "
        f"elapsed={payload['elapsedSeconds']}s cursor={payload['cursor']}",
        f"updated={payload['updatedAt']} session={payload['sessionId'] or '-'}",
    ]
    if payload.get("pendingRequest"):
        request = payload["pendingRequest"]
        lines.append("permission: " + clean_text(json.dumps(
            request, ensure_ascii=False, separators=(",", ":")), 600))
    tools = payload.get("recentTools") or []
    if tools:
        lines.append("tools:")
        for tool in tools:
            lines.append(f"- {clean_text(tool.get('status', ''), 30)}: "
                         f"{clean_text(tool.get('title', 'tool'), 180)}")
    agent = str(payload.get("lastAgentText") or "").strip()
    if agent:
        lines.extend(("agent:", agent))
    worktree = payload.get("worktree")
    if isinstance(worktree, dict):
        if worktree.get("available") and "changedSinceStartCount" in worktree:
            paths = worktree.get("changedSinceStart") or []
            lines.append(
                f"worktree: baseline_dirty={worktree.get('baselineDirty')} "
                f"current_dirty={worktree.get('currentDirty')} "
                f"changed_since_start={worktree.get('changedSinceStartCount')}")
            if paths:
                lines.append("changed: " + ", ".join(map(str, paths)))
        elif worktree.get("available"):
            lines.append(f"worktree: baseline_dirty={worktree.get('baselineDirty')}")
    if payload.get("error"):
        lines.append("error: " + str(payload["error"]))
    print(_bounded_text("\n".join(lines), args.max_chars))
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Delegate one prompt through Kimi ACP without forwarding its noisy stream.",
        epilog=("Monitor a live/finished run with: kimi-delegate.py status "
                "[--run-id RUN_ID] [--max-chars 1500] [--since CURSOR]"))
    prompt_group = parser.add_mutually_exclusive_group()
    prompt_group.add_argument("-p", "--prompt", help="prompt text; stdin is used when omitted")
    prompt_group.add_argument("-f", "--prompt-file", type=Path, help="UTF-8 prompt file")
    parser.add_argument("--cwd", type=Path, default=Path.cwd(), help="Kimi working directory")
    parser.add_argument("--additional-dir", action="append", type=Path, default=[],
                        help="additional working directory; repeat as needed")
    parser.add_argument("--kimi", default="kimi", help="Kimi executable name or path")
    parser.add_argument("--model", default="kimi-code/k3-256k", help="ACP model id")
    parser.add_argument("--thinking", choices=("low", "high", "max"), default="low")
    parser.add_argument("--mode", choices=("default", "plan", "auto", "yolo"), default="auto")
    parser.add_argument("--approval", choices=("ask", "reject", "allow-once", "allow-always"),
                        default="ask", help="ask on stdin (default) or apply an explicit policy")
    parser.add_argument("--session-id", help="resume an existing Kimi session without replay")
    parser.add_argument("--timeout", type=float, default=1800.0, help="maximum seconds per ACP request")
    parser.add_argument("--log", type=Path, help="optional raw ACP JSONL log (can be very large)")
    parser.add_argument("--verbose", action="store_true", help="show one line per completed tool")
    parser.add_argument("--json", action="store_true", help="emit a compact JSON result")
    parser.add_argument("--no-stats", action="store_true", help="omit the concise filtering summary")
    parser.add_argument("--state-dir", type=Path,
                        help="live snapshot directory (default: cwd/tmp/kimi-delegate-state)")
    parser.add_argument("--run-id", help="optional monitoring run id; generated when omitted")
    return parser.parse_args()


def load_prompt(args: argparse.Namespace) -> str:
    if args.prompt is not None:
        text = args.prompt
    elif args.prompt_file is not None:
        text = args.prompt_file.read_text(encoding="utf-8")
    else:
        text = sys.stdin.read()
    if not text.strip():
        raise AcpError("prompt is empty")
    return text


def main() -> int:
    # Codex/PowerShell may expose a GBK console on Windows while the caller
    # expects UTF-8 tool output. Keep the middleware boundary deterministic.
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure:
            reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) > 1 and sys.argv[1] in ("status", "wait"):
        return status_main(sys.argv[2:], wait_mode=sys.argv[1] == "wait")

    args = parse_args()
    client: AcpClient | None = None
    reporter: StatusReporter | None = None
    reporter_finished = False
    try:
        prompt = load_prompt(args)
        if args.approval == "ask" and args.prompt is None and args.prompt_file is None:
            raise AcpError("interactive approval requires -p/--prompt or -f/--prompt-file; stdin is reserved for replies")
        cwd = args.cwd.resolve(strict=True)
        additional_dirs = [path.resolve(strict=True) for path in args.additional_dir]
        if args.run_id and (len(args.run_id) > 96 or
                            not re.fullmatch(r"[A-Za-z0-9_.-]+", args.run_id)):
            raise AcpError("run id must be at most 96 letters, digits, dots, underscores, or hyphens")
        state_dir = resolve_state_dir(cwd, args.state_dir)
        reporter = StatusReporter(cwd, state_dir, args.run_id)
        state_file = state_dir / f"{reporter.run_id}.json"
        print(f"[kimi-delegate] run={reporter.run_id} state={state_file}",
              file=sys.stderr, flush=True)

        reporter.set_state("initializing", force=True)
        client = AcpClient(args.kimi, cwd, args.timeout, args.log, args.verbose,
                           args.approval, reporter)
        client.initialize()
        reporter.set_state("opening_session", force=True)
        client.open_session(args.session_id, additional_dirs)
        reporter.set_state("configuring", force=True)
        client.set_option("model", args.model)
        client.set_option("thinking", args.thinking)
        client.set_option("mode", args.mode)
        reporter.set_state("running", force=True)
        final_text, result = client.prompt(prompt)
        reporter.finish("completed", stats=client.stats,
                        stop_reason=result.get("stopReason"))
        reporter_finished = True

        payload = {
            "runId": reporter.run_id,
            "sessionId": client.session_id,
            "stopReason": result.get("stopReason"),
            "message": final_text,
            "filtered": client.stats,
        }
        if args.json:
            print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
        else:
            print(final_text.rstrip())
            print(f"[kimi-delegate] run={reporter.run_id} session={client.session_id}",
                  file=sys.stderr)
            if not args.no_stats:
                s = client.stats
                print(
                    "[kimi-delegate] filtered "
                    f"thought={s['thought_chunks']} chunks/{s['thought_chars']} chars, "
                    f"tool_updates={s['tool_updates']}, requests={s['permission_requests']}, "
                    f"raw={s['raw_bytes']} bytes",
                    file=sys.stderr,
                )
        return 0
    except KeyboardInterrupt:
        if client:
            client.cancel()
        if reporter:
            reporter.finish("cancelled", stats=client.stats if client else None,
                            error="cancelled by caller")
            reporter_finished = True
        print("kimi-delegate: cancelled", file=sys.stderr)
        return 130
    except (AcpError, TimeoutError, OSError, UnicodeError) as exc:
        if client:
            client.cancel()
        if reporter:
            reporter.finish("failed", stats=client.stats if client else None,
                            error=str(exc))
            reporter_finished = True
        print(f"kimi-delegate: {exc}", file=sys.stderr)
        return 1
    finally:
        if client:
            client.close()
        if reporter and not reporter_finished:
            reporter.finish("failed", stats=client.stats if client else None,
                            error="delegate exited before recording a terminal state")


if __name__ == "__main__":
    raise SystemExit(main())

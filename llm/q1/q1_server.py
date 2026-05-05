#!/usr/bin/env python3
import argparse
import hashlib
import errno
import json
import os
import queue
import re
import signal
import shutil
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse
import urllib.error
import urllib.request

MODEL_ID = "mlx-community/Qwen2.5-Coder-32B-Instruct-8bit"
SERVER_START = time.time()
MAX_CACHE_ENTRIES = 64
MAX_JOB_HISTORY = 200
DASHBOARD = None
STDERR_LOG_HANDLE = None
ANSI_RE = re.compile(r"\033\[[0-9;?]*[A-Za-z]")


class TerminalDashboard:
    def __init__(self, enabled):
        self.enabled = enabled
        self.lock = threading.Lock()
        self.height = 0
        self.last_message = ""
        self.recent_messages = []
        self.metrics = {}
        self.stats = {}
        self.stop_event = threading.Event()
        self.thread = None
        if self.enabled:
            sys.stdout.write("\033[?1049h\033[?25l\033[2J\033[H")
            sys.stdout.flush()
            self.thread = threading.Thread(target=self._render_loop, name="q1-dashboard", daemon=True)
            self.thread.start()
            self.update(message="SERVER dashboard starting")
            self.render()

    def close(self):
        if not self.enabled:
            return
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=1)
        with self.lock:
            sys.stdout.write("\033[?25h\033[?1049l")
            sys.stdout.flush()

    def update(self, message=None, metrics=None, stats=None):
        with self.lock:
            if message is not None:
                self.last_message = message
                timestamp = time.strftime("%H:%M:%S")
                self.recent_messages.append(f"{timestamp} {message}")
                self.recent_messages = self.recent_messages[-200:]
            if metrics is not None:
                self.metrics = metrics
            if stats is not None:
                self.stats = stats

    def _render_loop(self):
        while not self.stop_event.wait(1.0):
            self.render()
        self.render()

    def render(self):
        with self.lock:
            metrics = dict(self.metrics)
            stats = dict(self.stats)
            recent_messages = list(self.recent_messages)
            last_message = self.last_message

        handler = globals().get("Q1Handler")
        if handler and handler.job_manager:
            metrics = handler.metrics_snapshot()
            stats = handler.job_manager.stats()

        counts = metrics.get("request_counts", {})
        inference = metrics.get("inference_stats", {})
        jobs = stats.get("jobs", {})
        queue_depth = stats.get("queue_depth", 0)
        active_job_id = stats.get("active_job_id") or "-"
        active_elapsed = stats.get("active_job_elapsed_seconds")
        cache_entries = stats.get("cache_entries", 0)
        worker_alive = stats.get("worker_alive", True)
        worker = "ALIVE" if worker_alive else "DOWN"
        uptime = time.time() - SERVER_START
        avg = inference.get("average_seconds", 0.0)
        total = inference.get("total_seconds", 0.0)
        size = shutil.get_terminal_size((120, 32))
        width = size.columns
        height = size.lines
        if width < 80 or height < 20:
            warning = [
                "q1 model server",
                "Terminal too small for dashboard.",
                "Use at least 80 columns x 20 rows.",
                f"Current size: {width} columns x {height} rows.",
            ]
            sys.stdout.write("\033[H")
            sys.stdout.write("\n".join(line[:width].ljust(width) for line in warning[:height]))
            sys.stdout.flush()
            return
        log_height = max(4, height - 13)

        def fit(text):
            visible = ANSI_RE.sub("", text)
            if len(visible) > width:
                return visible[:width]
            return text + " " * (width - len(visible))

        def color(code, text):
            return f"\033[{code}m{text}\033[0m"

        def label(text):
            return color("38;5;208;1", text)

        def value(text):
            return color("38;5;231;1", text)

        def status(text):
            code = "38;5;46;1" if worker_alive else "38;5;196;1"
            return color(code, text)

        def count_cell(name, raw_value):
            if not name:
                return " " * 29
            return f"{name:<12} {value(f'{raw_value:>14}')}"

        def count_row(a_name, a_value, b_name="", b_value=""):
            return (
                "  "
                + count_cell(a_name, a_value)
                + "    "
                + count_cell(b_name, b_value)
            )

        def generate_row():
            left = count_cell("total", total_requests)
            right = count_cell("generate", generate_requests)
            return "  " + left + "    " + right + "    " + value(f"{generate_avg_seconds_text:>8}")

        def section_row(title):
            return fit(label(title.upper()))

        total_requests = counts.get("total", 0)
        generate_requests = counts.get("generate", 0)
        health_requests = counts.get("health", 0)
        job_requests = counts.get("jobs", 0)
        error_requests = counts.get("errors", 0)
        completed = inference.get("completed", 0)
        cached = inference.get("cached", 0)
        done_jobs = jobs.get("done", 0)
        failed_jobs = jobs.get("failed", 0)
        queued_jobs = jobs.get("queued", 0)
        running_jobs = jobs.get("running", 0)
        cancelled_jobs = jobs.get("cancelled", 0)
        error_rate = (error_requests / total_requests * 100) if total_requests else 0.0
        terminal_jobs = done_jobs + failed_jobs + cancelled_jobs
        success_rate = (done_jobs / terminal_jobs * 100) if terminal_jobs else 0.0
        active_elapsed_value = active_elapsed or 0.0
        uptime_text = format_duration(uptime, fractional_seconds=False)
        active_text = format_duration(active_elapsed_value)
        generate_avg_text = format_duration(avg)
        generate_avg_seconds_text = f"{avg:.2f}s"
        total_text = format_duration(total)
        status_text = "RUNNING" if worker_alive else "DOWN"
        lines = [
            fit(
                f"{label('q1')} model server   "
                f"{status(status_text):<7}   "
                f"uptime {value(f'{uptime_text:>10}')}   "
                f"active {value(f'{active_job_id:<12}')}   "
                f"Ctrl-C"
            ),
            fit(""),
            section_row("status"),
            fit(count_row("queue", queue_depth, "active", active_text)),
            fit(count_row("cache", f"{cache_entries}/{MAX_CACHE_ENTRIES}", "worker", worker)),
            fit(""),
            section_row("requests"),
            fit(
                generate_row()
            ),
            fit(
                count_row("health", health_requests, "job req", job_requests)
            ),
            fit(
                count_row("errors", error_requests, "error rate", f"{error_rate:.2f}%")
            ),
            fit(""),
            section_row("inference"),
            fit(
                count_row("completed", completed, "cached", cached)
            ),
            fit(
                count_row("generate avg", generate_avg_text, "total", total_text)
            ),
            fit(""),
            section_row("jobs"),
            fit(
                count_row("queued", queued_jobs, "running", running_jobs)
            ),
            fit(
                count_row("done", done_jobs, "failed", failed_jobs)
            ),
            fit(
                count_row("cancelled", cancelled_jobs, "success", f"{success_rate:.1f}%")
            ),
            fit(""),
            section_row("recent"),
        ]

        visible_logs = recent_messages[-log_height:]
        for index, message in enumerate(visible_logs, start=max(1, len(recent_messages) - len(visible_logs) + 1)):
            timestamp, _, detail = message.partition(" ")
            event, _, rest = detail.partition(" ")
            lines.append(fit(f"  {index:4d}  {timestamp:<8} {event:<12} {rest}"))
        while len(lines) < height - 1:
            lines.append(fit(""))
        lines.append(fit(f"{label('LAST')} {last_message}"))

        sys.stdout.write("\033[H")
        sys.stdout.write("\n".join(lines[:height]))
        sys.stdout.flush()


def log(message):
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {message}"
    if DASHBOARD:
        DASHBOARD.update(message=message)
        if DASHBOARD.enabled:
            return
    print(line, flush=True)


def redirect_stderr_for_dashboard():
    global STDERR_LOG_HANDLE
    if STDERR_LOG_HANDLE:
        return
    log_path = os.path.join(os.getcwd(), "q1_server.stderr.log")
    STDERR_LOG_HANDLE = open(log_path, "a", buffering=1)
    STDERR_LOG_HANDLE.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] stderr redirected from q1 dashboard\n")
    sys.stderr.flush()
    os.dup2(STDERR_LOG_HANDLE.fileno(), sys.stderr.fileno())


def format_duration(seconds, fractional_seconds=True):
    try:
        seconds = float(seconds)
    except (TypeError, ValueError):
        seconds = 0.0
    seconds = max(0.0, seconds)
    if seconds < 60:
        if fractional_seconds:
            return f"{seconds:.2f}s"
        return f"{int(seconds)}s"
    total_seconds = int(round(seconds))
    minutes, second = divmod(total_seconds, 60)
    if minutes < 60:
        return f"{minutes:02d}:{second:02d}"
    hour, minute = divmod(minutes, 60)
    return f"{hour:02d}:{minute:02d}:{second:02d}"


class ReusableHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def listener_pids(port):
    try:
        result = subprocess.run(
            ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-t"],
            capture_output=True,
            text=True,
            timeout=2,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if result.returncode not in {0, 1}:
        return []
    pids = []
    for line in result.stdout.splitlines():
        try:
            pids.append(int(line.strip()))
        except ValueError:
            continue
    return sorted(set(pids))


def process_command(pid):
    try:
        result = subprocess.run(
            ["ps", "-p", str(pid), "-o", "command="],
            capture_output=True,
            text=True,
            timeout=2,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def terminate_existing_q1_server(port, timeout=5):
    pids = [
        pid for pid in listener_pids(port)
        if pid != os.getpid() and "q1_server.py" in process_command(pid)
    ]
    if not pids:
        return []

    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
            log(f"SERVER stopping existing q1 pid={pid}")
        except ProcessLookupError:
            pass

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        alive = [pid for pid in pids if os.path.exists(f"/proc/{pid}") or process_command(pid)]
        if not alive:
            return pids
        if not any(pid in listener_pids(port) for pid in alive):
            return pids
        time.sleep(0.2)

    for pid in pids:
        if process_command(pid):
            try:
                os.kill(pid, signal.SIGKILL)
                log(f"SERVER force killed existing q1 pid={pid}")
            except ProcessLookupError:
                pass
    return pids


def terminate_health_checked_listener(host, port, timeout=5):
    health = existing_server_health(host, port)
    if not (health and health.get("ok")):
        return []
    killed = terminate_existing_q1_server(port, timeout=timeout)
    if killed:
        return killed
    pids = [pid for pid in listener_pids(port) if pid != os.getpid()]
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
            log(f"SERVER stopping health-checked listener pid={pid}")
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not any(pid in listener_pids(port) for pid in pids):
            return pids
        time.sleep(0.2)
    for pid in pids:
        if pid in listener_pids(port):
            try:
                os.kill(pid, signal.SIGKILL)
                log(f"SERVER force killed health-checked listener pid={pid}")
            except ProcessLookupError:
                pass
    return pids


def bind_server(host, port, attempts=10):
    last_error = None
    for attempt in range(1, attempts + 1):
        try:
            return ReusableHTTPServer((host, port), Q1Handler)
        except OSError as exc:
            if exc.errno != errno.EADDRINUSE:
                raise
            last_error = exc
            health = existing_server_health(host, port)
            if health and health.get("ok"):
                log(f"SERVER existing q1 detected url={health_url(host, port)} model={health.get('model', '(missing)')} attempt={attempt}")
                terminate_health_checked_listener(host, port)
            else:
                log(f"SERVER bind retry port busy host={host} port={port} attempt={attempt}: {exc}")
            time.sleep(0.5)
    raise last_error


def health_url(host, port):
    check_host = "127.0.0.1" if host in {"", "0.0.0.0"} else host
    return f"http://{check_host}:{port}/health"


def existing_server_health(host, port, timeout=2):
    try:
        with urllib.request.urlopen(health_url(host, port), timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except (OSError, urllib.error.URLError, json.JSONDecodeError):
        return None


def install_shutdown_handlers(server):
    state = {"requested": False}

    def request_shutdown(signum, frame):
        if state["requested"]:
            return
        state["requested"] = True
        name = signal.Signals(signum).name
        log(f"SERVER shutdown requested signal={name}")
        threading.Thread(target=server.shutdown, name="q1-server-shutdown", daemon=True).start()

    signal.signal(signal.SIGTERM, request_shutdown)
    signal.signal(signal.SIGINT, request_shutdown)


class Q1ModelService:
    def __init__(self, model_id):
        import mlx_lm

        self.mlx_lm = mlx_lm
        self.model_id = model_id
        start = time.time()
        log(f"LOAD start model={model_id}")
        self.model, self.tokenizer = mlx_lm.load(model_id)
        log(f"LOAD done elapsed={time.time() - start:.2f}s")

    def iter_generate(self, messages, max_tokens):
        try:
            start = time.monotonic()
            prompt = self.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
            log(f"GENERATE start messages={len(messages)} prompt_chars={len(prompt)} max_tokens={max_tokens}")
            chunks = 0
            output_chars = 0
            for response in self.mlx_lm.stream_generate(self.model, self.tokenizer, prompt, max_tokens=max_tokens):
                text = response.text
                chunks += 1
                output_chars += len(text)
                if chunks % 50 == 0:
                    elapsed_seconds = time.monotonic() - start
                    log(f"GENERATE progress chunks={chunks} elapsed_seconds={elapsed_seconds:.2f}")
                yield text
            elapsed_seconds = time.monotonic() - start
            chunks_per_second = chunks / elapsed_seconds if elapsed_seconds > 0 else 0
            log(f"GENERATE done output_chars={output_chars} chunks={chunks} elapsed_seconds={elapsed_seconds:.2f} chunks_per_second={chunks_per_second:.2f}")
        except Exception as e:
            log(f"GENERATE error: {str(e)}")
            raise

    def generate(self, messages, max_tokens):
        return "".join(self.iter_generate(messages, max_tokens))


class Q1JobManager:
    def __init__(self, service, max_cache_entries=MAX_CACHE_ENTRIES, max_job_history=MAX_JOB_HISTORY):
        self.service = service
        self.max_cache_entries = max_cache_entries
        self.max_job_history = max_job_history
        self.jobs = {}
        self.job_events = {}
        self.cache = {}
        self.cache_order = []
        self.inflight_by_key = {}
        self.active_job_id = None
        self.lock = threading.RLock()
        self.work_queue = queue.Queue()
        self.worker = threading.Thread(target=self._worker_loop, name="q1-generate-worker", daemon=True)
        self.worker.start()

    def cache_key(self, messages, max_tokens):
        payload = {
            "model": self.service.model_id,
            "messages": messages,
            "max_tokens": max_tokens,
        }
        encoded = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()

    def submit(self, messages, max_tokens, request_id=None, stream=False, allow_join=True):
        key = self.cache_key(messages, max_tokens)
        now = time.time()
        with self.lock:
            if key in self.cache:
                job_id = uuid.uuid4().hex[:12]
                cached = self.cache[key]
                job = {
                    "id": job_id,
                    "status": "done",
                    "created_at": now,
                    "started_at": now,
                    "finished_at": now,
                    "elapsed_seconds": 0.0,
                    "request_id": request_id,
                    "max_tokens": max_tokens,
                    "cached": True,
                    "streaming": stream,
                    "cache_key": key,
                    "text": cached["text"],
                    "output_chars": len(cached["text"]),
                }
                self.jobs[job_id] = job
                self._prune_locked()
                log(f"JOB {job_id} cache hit request_id={request_id}")
                Q1Handler.record_generation_result(0.0, cached=True)
                Q1Handler.update_dashboard()
                return job_id

            existing_id = self.inflight_by_key.get(key)
            if allow_join and existing_id and existing_id in self.jobs:
                log(f"JOB {existing_id} joined duplicate request_id={request_id}")
                return existing_id

            job_id = uuid.uuid4().hex[:12]
            job = {
                "id": job_id,
                "status": "queued",
                "created_at": now,
                "started_at": None,
                "finished_at": None,
                "elapsed_seconds": None,
                "request_id": request_id,
                "max_tokens": max_tokens,
                "cached": False,
                "streaming": stream,
                "cache_key": key,
                "messages": messages,
                "output_chars": None,
            }
            if stream:
                job["stream_queue"] = queue.Queue()
            self.jobs[job_id] = job
            self.job_events[job_id] = threading.Event()
            if allow_join:
                self.inflight_by_key[key] = job_id
            self.work_queue.put(job_id)
            self._prune_locked()
            log(f"JOB {job_id} queued request_id={request_id} queue_depth={self.work_queue.qsize()}")
            return job_id

    def wait(self, job_id, timeout=None):
        with self.lock:
            event = self.job_events.get(job_id)
            if event is None:
                job = self.jobs.get(job_id)
                if job and job.get("status") in {"done", "failed", "cancelled"}:
                    return self._public_job_locked(job, include_text=True)
                raise KeyError(job_id)
        event.wait(timeout=timeout)
        return self.job_snapshot(job_id, include_text=True)

    def cancel(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if not job:
                return None
            if job["status"] != "queued":
                return self._public_job_locked(job, include_text=True)
            job["status"] = "cancelled"
            job["finished_at"] = time.time()
            job["elapsed_seconds"] = 0.0
            cache_key = job.get("cache_key")
            if self.inflight_by_key.get(cache_key) == job_id:
                self.inflight_by_key.pop(cache_key, None)
            event = self.job_events.get(job_id)
            if event:
                event.set()
            return self._public_job_locked(job, include_text=True)

    def job_snapshot(self, job_id, include_text=False):
        with self.lock:
            job = self.jobs.get(job_id)
            if not job:
                return None
            return self._public_job_locked(job, include_text=include_text)

    def stream_queue(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if not job or "stream_queue" not in job:
                raise KeyError(job_id)
            return job["stream_queue"]

    def list_jobs(self, limit=20):
        with self.lock:
            jobs = sorted(self.jobs.values(), key=lambda item: item["created_at"], reverse=True)[:limit]
            return [self._public_job_locked(job, include_text=False) for job in jobs]

    def stats(self):
        with self.lock:
            counts = {}
            for job in self.jobs.values():
                counts[job["status"]] = counts.get(job["status"], 0) + 1
            active_elapsed = 0.0
            if self.active_job_id and self.active_job_id in self.jobs:
                started_at = self.jobs[self.active_job_id].get("started_at")
                if started_at:
                    active_elapsed = round(time.time() - started_at, 2)
            return {
                "queue_depth": self.work_queue.qsize(),
                "active_job_id": self.active_job_id,
                "active_job_elapsed_seconds": active_elapsed,
                "jobs": counts,
                "cache_entries": len(self.cache),
                "worker_alive": self.worker.is_alive(),
            }

    def _worker_loop(self):
        while True:
            job_id = self.work_queue.get()
            try:
                self._run_job(job_id)
            finally:
                self.work_queue.task_done()

    def _run_job(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if not job:
                return
            if job["status"] == "cancelled":
                return
            job["status"] = "running"
            job["started_at"] = time.time()
            self.active_job_id = job_id
            messages = job["messages"]
            max_tokens = job["max_tokens"]
            cache_key = job["cache_key"]
            stream_queue = job.get("stream_queue")
        log(f"JOB {job_id} running")
        try:
            chunks = []
            if stream_queue:
                stream_queue.put({"event": "start", "job_id": job_id})
            for chunk in self.service.iter_generate(messages, max_tokens):
                chunks.append(chunk)
                if stream_queue:
                    stream_queue.put({"event": "chunk", "text": chunk})
            text = "".join(chunks)
            finished_at = time.time()
            with self.lock:
                job = self.jobs.get(job_id)
                if not job:
                    return
                job["status"] = "done"
                job["finished_at"] = finished_at
                job["elapsed_seconds"] = round(finished_at - job["started_at"], 2)
                job["text"] = text
                job["output_chars"] = len(text)
                job.pop("messages", None)
                self._put_cache_locked(cache_key, text)
                if self.inflight_by_key.get(cache_key) == job_id:
                    self.inflight_by_key.pop(cache_key, None)
                self.active_job_id = None
                event = self.job_events.get(job_id)
                if event:
                    event.set()
                elapsed_seconds = job["elapsed_seconds"]
            if stream_queue:
                stream_queue.put({"event": "done", "job_id": job_id, "elapsed_seconds": elapsed_seconds})
            Q1Handler.record_generation_result(elapsed_seconds, cached=False)
            Q1Handler.update_dashboard()
            log(f"JOB {job_id} done elapsed_seconds={elapsed_seconds:.2f}")
        except Exception as exc:
            finished_at = time.time()
            stream_queue = None
            with self.lock:
                job = self.jobs.get(job_id)
                if job:
                    stream_queue = job.get("stream_queue")
                    job["status"] = "failed"
                    job["finished_at"] = finished_at
                    job["elapsed_seconds"] = round(finished_at - job["started_at"], 2) if job["started_at"] else None
                    job["error"] = str(exc)
                    job.pop("messages", None)
                    failed_key = job.get("cache_key")
                    if self.inflight_by_key.get(failed_key) == job_id:
                        self.inflight_by_key.pop(failed_key, None)
                    event = self.job_events.get(job_id)
                    if event:
                        event.set()
                self.active_job_id = None
            if stream_queue:
                stream_queue.put({"event": "error", "job_id": job_id, "error": str(exc)})
            log(f"JOB {job_id} failed error={exc}")

    def _put_cache_locked(self, cache_key, text):
        self.cache[cache_key] = {"text": text, "stored_at": time.time()}
        if cache_key in self.cache_order:
            self.cache_order.remove(cache_key)
        self.cache_order.append(cache_key)
        while len(self.cache_order) > self.max_cache_entries:
            oldest = self.cache_order.pop(0)
            self.cache.pop(oldest, None)

    def _prune_locked(self):
        if len(self.jobs) <= self.max_job_history:
            return
        terminal = [
            job for job in self.jobs.values()
            if job["status"] in {"done", "failed", "cancelled"}
        ]
        terminal.sort(key=lambda item: item["created_at"])
        for job in terminal[:len(self.jobs) - self.max_job_history]:
            self.jobs.pop(job["id"], None)
            self.job_events.pop(job["id"], None)

    def _public_job_locked(self, job, include_text=False):
        public = {
            key: value for key, value in job.items()
            if key not in {"messages", "cache_key", "text", "stream_queue"}
        }
        public["queue_position"] = self._queue_position_locked(job["id"]) if job["status"] == "queued" else None
        if include_text and "text" in job:
            public["text"] = job["text"]
        return public

    def _queue_position_locked(self, job_id):
        queued = [
            job for job in self.jobs.values()
            if job["status"] == "queued"
        ]
        queued.sort(key=lambda item: item["created_at"])
        for index, job in enumerate(queued, start=1):
            if job["id"] == job_id:
                return index
        return None


class Q1Handler(BaseHTTPRequestHandler):
    service = None
    job_manager = None
    request_count = 0
    total_requests = 0
    health_requests = 0
    generate_requests = 0
    job_requests = 0
    error_requests = 0
    completed_generations = 0
    cached_generations = 0
    total_inference_seconds = 0.0
    counter_lock = threading.Lock()

    @classmethod
    def record_request(cls, kind):
        with cls.counter_lock:
            cls.total_requests += 1
            if kind == "health":
                cls.health_requests += 1
            elif kind == "generate":
                cls.generate_requests += 1
                cls.request_count = cls.generate_requests
            elif kind == "job":
                cls.job_requests += 1
            elif kind == "error":
                cls.error_requests += 1
            return cls.metrics_snapshot_locked()

    @classmethod
    def record_error(cls):
        with cls.counter_lock:
            cls.error_requests += 1
            return cls.metrics_snapshot_locked()

    @classmethod
    def record_generation_result(cls, elapsed_seconds, cached=False):
        with cls.counter_lock:
            if cached:
                cls.cached_generations += 1
            else:
                try:
                    elapsed = float(elapsed_seconds)
                except (TypeError, ValueError):
                    elapsed = 0.0
                cls.completed_generations += 1
                cls.total_inference_seconds += max(0.0, elapsed)
            return cls.metrics_snapshot_locked()

    @classmethod
    def metrics_snapshot(cls):
        with cls.counter_lock:
            return cls.metrics_snapshot_locked()

    @classmethod
    def metrics_snapshot_locked(cls):
        average = cls.total_inference_seconds / cls.completed_generations if cls.completed_generations else 0.0
        return {
            "request_counts": {
                "total": cls.total_requests,
                "health": cls.health_requests,
                "generate": cls.generate_requests,
                "jobs": cls.job_requests,
                "errors": cls.error_requests,
            },
            "inference_stats": {
                "completed": cls.completed_generations,
                "cached": cls.cached_generations,
                "total_seconds": round(cls.total_inference_seconds, 2),
                "average_seconds": round(average, 2),
                "average_generate_seconds": round(average, 2),
            },
        }

    @classmethod
    def metrics_log(cls):
        metrics = cls.metrics_snapshot()
        counts = metrics["request_counts"]
        inference = metrics["inference_stats"]
        return (
            "counts "
            f"total={counts['total']} generate={counts['generate']} health={counts['health']} "
            f"jobs={counts['jobs']} errors={counts['errors']} "
            "inference "
            f"completed={inference['completed']} cached={inference['cached']} "
            f"avg_seconds={inference['average_seconds']:.2f} total_seconds={inference['total_seconds']:.2f}"
        )

    @classmethod
    def update_dashboard(cls):
        if DASHBOARD:
            stats = cls.job_manager.stats() if cls.job_manager else {}
            DASHBOARD.update(metrics=cls.metrics_snapshot(), stats=stats)

    def assign_request_id(self):
        self.request_id = uuid.uuid4().hex[:8]
        return self.request_id

    def do_GET(self):
        request_id = self.assign_request_id()
        try:
            path = urlparse(self.path).path
            if path == "/health":
                metrics = self.record_request("health")
                stats = self.job_manager.stats()
                self.update_dashboard()
                log(f"REQUEST {request_id} HEALTH ok {self.metrics_log()}")
                self.send_json(200, {
                    "ok": True,
                    "model": self.service.model_id,
                    "uptime_seconds": round(time.time() - SERVER_START, 2),
                    "requests": metrics["request_counts"]["generate"],
                    "request_counts": metrics["request_counts"],
                    "inference_stats": metrics["inference_stats"],
                    "average_generate_seconds": metrics["inference_stats"]["average_generate_seconds"],
                    "queue_depth": stats["queue_depth"],
                    "active_job_id": stats["active_job_id"],
                    "jobs": stats["jobs"],
                    "cache_entries": stats["cache_entries"],
                    "worker_alive": stats["worker_alive"],
                    "streaming_supported": True,
                })
                return
            if path == "/jobs":
                self.record_request("job")
                self.update_dashboard()
                log(f"REQUEST {request_id} JOBS list {self.metrics_log()}")
                self.send_json(200, {"ok": True, "jobs": self.job_manager.list_jobs()})
                return
            if path.startswith("/jobs/"):
                self.record_request("job")
                job_id = path.split("/", 2)[2]
                job = self.job_manager.job_snapshot(job_id, include_text=True)
                if not job:
                    self.record_error()
                    self.update_dashboard()
                    self.send_json(404, {"ok": False, "error": "job not found"})
                    return
                self.update_dashboard()
                log(f"REQUEST {request_id} JOBS get job_id={job_id} {self.metrics_log()}")
                self.send_json(200, {"ok": True, "job": job})
                return
            self.record_request("error")
            self.update_dashboard()
            log(f"REQUEST {request_id} GET {self.path} status=404")
            self.send_json(404, {"ok": False, "error": "not found"})
        except Exception as e:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST {request_id} Error in do_GET: {str(e)}")
            self.send_json(500, {"ok": False, "error": "internal server error"})

    def do_POST(self):
        request_id = self.assign_request_id()
        path = urlparse(self.path).path
        if path == "/generate":
            self.handle_generate(request_id)
            return
        if path == "/generate_stream":
            self.handle_generate_stream(request_id)
            return
        if path == "/jobs":
            self.handle_create_job(request_id)
            return
        if path.startswith("/jobs/") and path.endswith("/cancel"):
            self.record_request("job")
            job_id = path.split("/")[2]
            job = self.job_manager.cancel(job_id)
            if not job:
                self.record_error()
                self.update_dashboard()
                self.send_json(404, {"ok": False, "error": "job not found"})
                return
            self.update_dashboard()
            log(f"REQUEST {request_id} JOBS cancel job_id={job_id} {self.metrics_log()}")
            self.send_json(200, {"ok": True, "job": job})
            return
        self.record_request("error")
        self.update_dashboard()
        log(f"REQUEST {request_id} POST {self.path} status=404")
        self.send_json(404, {"ok": False, "error": "not found"})

    def handle_generate(self, request_id):
        try:
            self.record_request("generate")
            self.update_dashboard()
            payload = self.read_json_payload()
            messages, max_tokens = self.validate_generate_payload(payload, request_id)
            if messages is None:
                self.record_error()
                self.update_dashboard()
                return
            job_id = self.job_manager.submit(messages, max_tokens, request_id=request_id)
            job = self.job_manager.wait(job_id)
            if job["status"] == "done":
                self.update_dashboard()
                log(
                    f"REQUEST #{request_id} success job_id={job_id} "
                    f"cached={job.get('cached')} elapsed_seconds={job.get('elapsed_seconds')} {self.metrics_log()}"
                )
                self.send_json(200, {
                    "ok": True,
                    "text": job.get("text", ""),
                    "job_id": job_id,
                    "cached": job.get("cached", False),
                    "elapsed_seconds": job.get("elapsed_seconds"),
                })
                return
            status = 500 if job["status"] == "failed" else 409
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST #{request_id} failed job_id={job_id} status={job['status']} {self.metrics_log()}")
            self.send_json(status, {"ok": False, "error": job.get("error", job["status"]), "job": job})
        except json.JSONDecodeError as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(400, {"ok": False, "error": "Invalid JSON payload"})
        except KeyError as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(400, {"ok": False, "error": "Missing required field in payload"})
        except Exception as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(500, {"ok": False, "error": str(exc)})

    def handle_generate_stream(self, request_id):
        try:
            self.record_request("generate")
            self.update_dashboard()
            payload = self.read_json_payload()
            messages, max_tokens = self.validate_generate_payload(payload, request_id)
            if messages is None:
                self.record_error()
                self.update_dashboard()
                return
            job_id = self.job_manager.submit(
                messages,
                max_tokens,
                request_id=request_id,
                stream=True,
                allow_join=False,
            )
            job = self.job_manager.job_snapshot(job_id, include_text=True)
            self.send_stream_headers()
            self.send_stream_event({
                "event": "accepted",
                "job_id": job_id,
                "cached": job.get("cached", False),
                "queue_position": job.get("queue_position"),
            })
            if job["status"] == "done":
                self.send_stream_event({"event": "chunk", "text": job.get("text", "")})
                self.send_stream_event({
                    "event": "done",
                    "job_id": job_id,
                    "elapsed_seconds": job.get("elapsed_seconds"),
                    "cached": True,
                })
                self.update_dashboard()
                log(
                    f"REQUEST #{request_id} stream done job_id={job_id} cached=True "
                    f"elapsed_seconds={job.get('elapsed_seconds')} {self.metrics_log()}"
                )
                return
            stream_queue = self.job_manager.stream_queue(job_id)
            while True:
                event = stream_queue.get()
                self.send_stream_event(event)
                if event.get("event") == "done":
                    self.update_dashboard()
                    log(
                        f"REQUEST #{request_id} stream done job_id={job_id} "
                        f"elapsed_seconds={event.get('elapsed_seconds')} {self.metrics_log()}"
                    )
                    break
                if event.get("event") == "error":
                    self.record_error()
                    self.update_dashboard()
                    log(f"REQUEST #{request_id} stream failed job_id={job_id} {self.metrics_log()}")
                    break
        except (BrokenPipeError, ConnectionResetError):
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST #{request_id} stream client disconnected")
        except json.JSONDecodeError as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(400, {"ok": False, "error": "Invalid JSON payload"})
        except KeyError as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(400, {"ok": False, "error": "Missing required field in payload"})
        except Exception as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            try:
                self.send_json(500, {"ok": False, "error": str(exc)})
            except (BrokenPipeError, ConnectionResetError):
                pass

    def handle_create_job(self, request_id):
        try:
            self.record_request("job")
            self.update_dashboard()
            payload = self.read_json_payload()
            messages, max_tokens = self.validate_generate_payload(payload, request_id)
            if messages is None:
                self.record_error()
                self.update_dashboard()
                return
            job_id = self.job_manager.submit(messages, max_tokens, request_id=request_id)
            job = self.job_manager.job_snapshot(job_id)
            log(f"REQUEST #{request_id} JOBS create job_id={job_id} {self.metrics_log()}")
            self.send_json(202, {"ok": True, "job_id": job_id, "job": job})
        except json.JSONDecodeError as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(400, {"ok": False, "error": "Invalid JSON payload"})
        except KeyError as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(400, {"ok": False, "error": "Missing required field in payload"})
        except Exception as exc:
            self.record_error()
            self.update_dashboard()
            log(f"REQUEST error {type(exc).__name__}: {exc}")
            self.send_json(500, {"ok": False, "error": str(exc)})

    def read_json_payload(self):
        length = int(self.headers.get("Content-Length", "0"))
        log(f"REQUEST #{self.request_id} {self.path} bytes={length}")
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def validate_generate_payload(self, payload, request_id):
        messages = payload["messages"]
        if not isinstance(messages, list):
            log(f"REQUEST #{request_id} validation error: messages must be a list")
            self.send_json(400, {"ok": False, "error": "messages must be a list"})
            return None, None
        max_tokens = int(payload.get("max_tokens", 512))
        if max_tokens < 1:
            log(f"REQUEST #{request_id} validation error: max_tokens must be >= 1")
            self.send_json(400, {"ok": False, "error": "max_tokens must be >= 1"})
            return None, None
        return messages, max_tokens

    def send_stream_headers(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        if hasattr(self, "request_id"):
            self.send_header("X-Q1-Request-Id", self.request_id)
        self.end_headers()

    def send_stream_event(self, event):
        line = json.dumps(event, ensure_ascii=False).encode("utf-8") + b"\n"
        self.wfile.write(line)
        self.wfile.flush()

    def log_message(self, format, *args):
        return

    def send_json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if hasattr(self, "request_id"):
            self.send_header("X-Q1-Request-Id", self.request_id)
        self.end_headers()
        self.wfile.write(body)


def main():
    global DASHBOARD
    try:
        parser = argparse.ArgumentParser()
        parser.add_argument("--host", default="127.0.0.1")
        parser.add_argument("--port", type=int, default=8765)
        parser.add_argument("--model", default=MODEL_ID)
        parser.add_argument(
            "--dashboard",
            choices=["auto", "always", "off"],
            default="auto",
            help="Show a fixed-position terminal dashboard. auto enables it only on a TTY.",
        )
        args = parser.parse_args()

        dashboard_enabled = args.dashboard == "always" or (args.dashboard == "auto" and sys.stdout.isatty())
        DASHBOARD = TerminalDashboard(dashboard_enabled)
        if DASHBOARD.enabled:
            redirect_stderr_for_dashboard()
        log(f"SERVER boot host={args.host} port={args.port}")
        server = bind_server(args.host, args.port)
        install_shutdown_handlers(server)
        Q1Handler.service = Q1ModelService(args.model)
        Q1Handler.job_manager = Q1JobManager(Q1Handler.service)
        Q1Handler.update_dashboard()
        log(f"SERVER ready url=http://{args.host}:{args.port}")
        try:
            server.serve_forever()
        finally:
            server.server_close()
            log("SERVER closed")
            if DASHBOARD:
                DASHBOARD.close()
    except Exception as e:
        log(f"SERVER error: {str(e)}")
        if DASHBOARD:
            DASHBOARD.close()
        raise


if __name__ == "__main__":
    main()

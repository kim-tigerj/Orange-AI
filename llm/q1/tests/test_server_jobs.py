import threading
import time
import unittest
from unittest import mock

import q1_server


def reset_handler_metrics():
    with q1_server.Q1Handler.counter_lock:
        q1_server.Q1Handler.request_count = 0
        q1_server.Q1Handler.total_requests = 0
        q1_server.Q1Handler.health_requests = 0
        q1_server.Q1Handler.generate_requests = 0
        q1_server.Q1Handler.job_requests = 0
        q1_server.Q1Handler.error_requests = 0
        q1_server.Q1Handler.completed_generations = 0
        q1_server.Q1Handler.cached_generations = 0
        q1_server.Q1Handler.total_inference_seconds = 0.0


class FakeService:
    model_id = "fake-model"

    def __init__(self):
        self.calls = 0
        self.lock = threading.Lock()

    def generate(self, messages, max_tokens):
        return "".join(self.iter_generate(messages, max_tokens))

    def iter_generate(self, messages, max_tokens):
        with self.lock:
            self.calls += 1
        time.sleep(0.02)
        yield messages[0]["content"]
        yield f":{max_tokens}"


class Q1ServerJobManagerTests(unittest.TestCase):
    def setUp(self):
        self.service = FakeService()
        self.manager = q1_server.Q1JobManager(self.service, max_cache_entries=2, max_job_history=10)

    def test_submit_runs_job_and_caches_identical_later_request(self):
        messages = [{"role": "user", "content": "hello"}]
        first_id = self.manager.submit(messages, 8, request_id="r1")
        first = self.manager.wait(first_id, timeout=2)

        self.assertEqual(first["status"], "done")
        self.assertEqual(first["text"], "hello:8")
        self.assertFalse(first["cached"])
        self.assertEqual(self.service.calls, 1)

        second_id = self.manager.submit(messages, 8, request_id="r2")
        second = self.manager.wait(second_id, timeout=2)

        self.assertEqual(second["status"], "done")
        self.assertEqual(second["text"], "hello:8")
        self.assertTrue(second["cached"])
        self.assertEqual(self.service.calls, 1)

    def test_duplicate_inflight_request_reuses_existing_job(self):
        messages = [{"role": "user", "content": "same"}]
        first_id = self.manager.submit(messages, 4, request_id="r1")
        second_id = self.manager.submit(messages, 4, request_id="r2")

        self.assertEqual(first_id, second_id)
        job = self.manager.wait(first_id, timeout=2)
        self.assertEqual(job["status"], "done")
        self.assertEqual(self.service.calls, 1)

    def test_stats_reports_queue_cache_and_worker(self):
        messages = [{"role": "user", "content": "stats"}]
        job_id = self.manager.submit(messages, 2)
        self.manager.wait(job_id, timeout=2)

        stats = self.manager.stats()
        self.assertTrue(stats["worker_alive"])
        self.assertEqual(stats["cache_entries"], 1)
        self.assertIn("done", stats["jobs"])

    def test_stream_job_emits_chunks_and_done_event(self):
        messages = [{"role": "user", "content": "stream"}]
        job_id = self.manager.submit(messages, 3, stream=True, allow_join=False)
        stream_queue = self.manager.stream_queue(job_id)
        events = []
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            event = stream_queue.get(timeout=deadline - time.monotonic())
            events.append(event)
            if event["event"] == "done":
                break

        self.assertEqual([event["event"] for event in events], ["start", "chunk", "chunk", "done"])
        self.assertEqual("".join(event.get("text", "") for event in events), "stream:3")
        job = self.manager.wait(job_id, timeout=2)
        self.assertEqual(job["status"], "done")


class Q1HandlerFixtureTests(unittest.TestCase):
    def setUp(self):
        reset_handler_metrics()

    def test_generate_stream_events_for_cached_job(self):
        handler = object.__new__(q1_server.Q1Handler)
        handler.request_id = "req1"
        handler.path = "/generate_stream"
        handler.rfile = None
        handler.wfile = mock.Mock()
        handler.send_response = mock.Mock()
        handler.send_header = mock.Mock()
        handler.end_headers = mock.Mock()
        handler.read_json_payload = mock.Mock(return_value={
            "messages": [{"role": "user", "content": "cached"}],
            "max_tokens": 5,
        })

        manager = q1_server.Q1JobManager(FakeService(), max_cache_entries=2, max_job_history=10)
        first_id = manager.submit([{"role": "user", "content": "cached"}], 5)
        manager.wait(first_id, timeout=2)
        handler.job_manager = manager

        handler.handle_generate_stream("req1")

        body = b"".join(call.args[0] for call in handler.wfile.write.call_args_list).decode("utf-8")
        events = [q1_server.json.loads(line) for line in body.splitlines()]
        self.assertEqual([event["event"] for event in events], ["accepted", "chunk", "done"])
        self.assertEqual(events[1]["text"], "cached:5")
        self.assertTrue(events[2]["cached"])
        handler.send_header.assert_any_call("Content-Type", "application/x-ndjson; charset=utf-8")


class Q1HandlerMetricsTests(unittest.TestCase):
    def setUp(self):
        reset_handler_metrics()

    def test_request_counts_and_inference_average_are_reported(self):
        q1_server.Q1Handler.record_request("health")
        q1_server.Q1Handler.record_request("generate")
        q1_server.Q1Handler.record_request("job")
        q1_server.Q1Handler.record_error()
        q1_server.Q1Handler.record_generation_result(2.0, cached=False)
        q1_server.Q1Handler.record_generation_result(4.0, cached=False)
        q1_server.Q1Handler.record_generation_result(0.0, cached=True)

        metrics = q1_server.Q1Handler.metrics_snapshot()

        self.assertEqual(metrics["request_counts"], {
            "total": 3,
            "health": 1,
            "generate": 1,
            "jobs": 1,
            "errors": 1,
        })
        self.assertEqual(metrics["inference_stats"], {
            "completed": 2,
            "cached": 1,
            "total_seconds": 6.0,
            "average_seconds": 3.0,
            "average_generate_seconds": 3.0,
        })

    def test_format_duration_uses_seconds_minutes_and_hours(self):
        self.assertEqual(q1_server.format_duration(12.345), "12.35s")
        self.assertEqual(q1_server.format_duration(12.345, fractional_seconds=False), "12s")
        self.assertEqual(q1_server.format_duration(65), "01:05")
        self.assertEqual(q1_server.format_duration(3661), "01:01:01")


if __name__ == "__main__":
    unittest.main()

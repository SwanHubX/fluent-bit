"""HTTP 数组必须整体接受或整体拒绝，失败后重试不能留下重复前缀。"""
import json
import time
from pathlib import Path

import pytest
import requests

from utils.test_service import FluentBitTestService


def service(tmp_path, events=65536, size=524288, workers=2):
    """使用独立端口和文件输出，保留真实 worker/ingress queue 路径。"""
    config = Path(__file__).resolve().parents[1] / "config/in_http_atomic.yaml"
    result = FluentBitTestService(str(config), extra_env={
        "ATOMIC_OUT": str(tmp_path), "ATOMIC_EVENTS": str(events),
        "ATOMIC_BYTES": str(size), "ATOMIC_WORKERS": str(workers),
    })
    result.start()
    result.wait_for_http_endpoint(f"http://127.0.0.1:{result.flb_listener_port}/health")
    return result


def rows(path):
    """只读输出中的业务对象，不把 HTTP 确认当作落盘确认。"""
    return [json.loads(line) for file in path.iterdir() if file.is_file()
            for line in file.read_text().splitlines()]


def post(instance, payload):
    """保留 HTTP 状态，成功不自动重发，临时背压才有界重试。"""
    url = f"http://127.0.0.1:{instance.flb_listener_port}/batch"
    return requests.post(url, json=payload, timeout=10)


def retry(instance, payload):
    """验证请求被 503 整批拒绝后仍可重试，不能掩盖其他错误。"""
    deadline = time.monotonic() + 10
    while True:
        response = post(instance, payload)
        if response.status_code != 503 or time.monotonic() >= deadline:
            assert response.status_code == 201, response.text
            return
        time.sleep(.1)


def test_bad_array(tmp_path):
    """数组尾部非法时，前面的合法对象也必须未被接收。"""
    instance = service(tmp_path)
    try:
        response = post(instance, [{"step": 1, "route": "a"}, {"step": 2, "route": "b"}, 42])
        assert response.status_code == 400
        time.sleep(1.5)
        assert rows(tmp_path) == []
        retry(instance, [{"step": 1, "route": "a"}, {"step": 2, "route": "b"}])
        instance.wait_for_condition(lambda: len(rows(tmp_path)) == 2)
        assert sorted(row["step"] for row in rows(tmp_path)) == [1, 2]
    finally:
        instance.stop()


def test_large_batch(tmp_path):
    """跨多个 Tag 的批次超过记录配额时全部拒绝，拆批后每条只出现一次。"""
    instance = service(tmp_path, events=2)
    payload = [{"step": 1, "route": "a"}, {"step": 2, "route": "b"}, {"step": 3, "route": "a"}]
    try:
        assert post(instance, payload).status_code == 413
        time.sleep(1.5)
        assert rows(tmp_path) == []
        retry(instance, payload[:2])
        retry(instance, payload[2:])
        instance.wait_for_condition(lambda: len(rows(tmp_path)) == 3)
        assert sorted(row["step"] for row in rows(tmp_path)) == [1, 2, 3]
        assert json.loads((tmp_path / "b").read_text())["step"] == 2
    finally:
        instance.stop()


def test_large_bytes(tmp_path):
    """无法装入字节配额的单批返回 413，不能让客户端永久重试 503。"""
    instance = service(tmp_path, size=100)
    try:
        assert post(instance, [{"message": "x" * 4096}]).status_code == 413
        time.sleep(1.5)
        assert rows(tmp_path) == []
        retry(instance, [{"step": 1}])
        instance.wait_for_condition(lambda: len(rows(tmp_path)) == 1)
    finally:
        instance.stop()


@pytest.mark.parametrize("workers", [1, 2])
def test_batch_ok(tmp_path, workers):
    """单 worker 与多 worker 都支持数组、空数组及默认 URI Tag。"""
    instance = service(tmp_path, workers=workers)
    try:
        assert post(instance, []).status_code == 201
        payload = [{"step": step, "message": "中文"} for step in range(1000)]
        retry(instance, payload)
        instance.wait_for_condition(lambda: len(rows(tmp_path)) == 1000)
        assert sorted(row["step"] for row in rows(tmp_path)) == list(range(1000))
        assert all(row["message"] == "中文" for row in rows(tmp_path))
    finally:
        instance.stop()

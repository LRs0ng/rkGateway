#!/usr/bin/env python3
"""Subscribe to miniGateway ADC events over MQTT and display a live waveform."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
import queue
import sys
from typing import Any, Optional

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import numpy as np

try:
    import paho.mqtt.client as mqtt
except ImportError as exc:  # pragma: no cover - exercised on the target host
    raise SystemExit(
        "缺少 paho-mqtt，请先安装：python3 -m pip install paho-mqtt"
    ) from exc


@dataclass
class Batch:
    values: np.ndarray
    timestamps_ns: np.ndarray
    sample_rate_hz: float


class WaveformState:
    def __init__(self, window_seconds: float) -> None:
        self.window_seconds = window_seconds
        self.values = np.empty(0, dtype=np.float64)
        self.timestamps_ns = np.empty(0, dtype=np.int64)
        self.sample_rate_hz = 0.0
        self.message_count = 0
        self.sample_count = 0
        self.bad_messages = 0
        self.queue_drops = 0
        self.last_event_id = ""

    def add(self, batch: Batch, event_id: str) -> None:
        if batch.values.size == 0:
            return
        self.values = np.concatenate((self.values, batch.values))
        self.timestamps_ns = np.concatenate((self.timestamps_ns, batch.timestamps_ns))
        self.sample_rate_hz = batch.sample_rate_hz
        self.message_count += 1
        self.sample_count += int(batch.values.size)
        self.last_event_id = event_id

        newest = int(self.timestamps_ns[-1])
        oldest = newest - int(self.window_seconds * 1_000_000_000.0)
        keep = self.timestamps_ns >= oldest
        self.values = self.values[keep]
        self.timestamps_ns = self.timestamps_ns[keep]


def positive_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0.0:
        raise argparse.ArgumentTypeError("必须是大于 0 的有限数")
    return number


def positive_int(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("必须是大于 0 的整数")
    return number


def reason_is_success(reason: Any) -> bool:
    # Paho MQTT 1.x supplies integer rc; MQTT 2.x supplies ReasonCode.
    try:
        return int(reason) == 0
    except (TypeError, ValueError):
        return str(reason).lower() in {"success", "normal connection"}


def parse_batch(
    payload: bytes,
    point: str,
    default_rate_hz: float,
) -> tuple[Batch, str]:
    import json

    document = json.loads(payload.decode("utf-8"))
    if not isinstance(document, dict):
        raise ValueError("event payload must be a JSON object")
    readings = document.get("readings")
    if not isinstance(readings, list):
        raise ValueError("event.readings must be an array")

    values: list[float] = []
    timestamps: list[int] = []
    for reading in readings:
        if not isinstance(reading, dict) or reading.get("point") != point:
            continue
        # miniGateway serializes Quality::Good as lowercase "good".  Be
        # tolerant of publishers that use "Good" or other letter casing.
        quality = reading.get("quality", "good")
        if not isinstance(quality, str) or quality.strip().lower() != "good":
            continue
        value = reading.get("value")
        timestamp = reading.get("source_time_ns")
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            continue
        if not math.isfinite(float(value)):
            continue
        values.append(float(value))
        if isinstance(timestamp, int) and not isinstance(timestamp, bool):
            timestamps.append(timestamp)
        else:
            timestamps.append(0)

    if not values:
        raise ValueError("event contains no valid ADC readings for point '" + point + "'")

    rate = default_rate_hz
    if len(timestamps) >= 2 and timestamps[1] > timestamps[0]:
        measured = 1_000_000_000.0 / float(timestamps[1] - timestamps[0])
        if math.isfinite(measured) and measured > 0.0:
            rate = measured
    if rate <= 0.0:
        raise ValueError("sample rate is unavailable")

    # If a sender omitted timestamps, synthesize a contiguous batch ending now.
    if any(timestamp == 0 for timestamp in timestamps):
        end_ns = timestamps[-1] if timestamps[-1] > 0 else 0
        if end_ns == 0:
            import time

            end_ns = time.time_ns()
        timestamps = [
            end_ns - int((len(values) - 1 - index) * 1_000_000_000.0 / rate)
            for index in range(len(values))
        ]

    return (
        Batch(
            values=np.asarray(values, dtype=np.float64),
            timestamps_ns=np.asarray(timestamps, dtype=np.int64),
            sample_rate_hz=rate,
        ),
        str(document.get("event_id", "")),
    )


def make_mqtt_client(args: argparse.Namespace, batches: queue.Queue[tuple[Batch, str]], state: WaveformState):
    try:
        client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=args.client_id,
        )
    except (AttributeError, TypeError):
        client = mqtt.Client(client_id=args.client_id)

    if args.username:
        client.username_pw_set(args.username, args.password or None)

    def on_connect(client: Any, _userdata: Any, _flags: Any, reason_code: Any, _properties: Any = None) -> None:
        if not reason_is_success(reason_code):
            print(f"MQTT 连接失败: {reason_code}", file=sys.stderr)
            return
        result, _ = client.subscribe(args.topic, qos=args.qos)
        if result != mqtt.MQTT_ERR_SUCCESS:
            print(f"MQTT 订阅失败: rc={result}", file=sys.stderr)
            return
        print(f"已订阅 {args.topic}")

    def on_message(_client: Any, _userdata: Any, message: Any) -> None:
        try:
            batch, event_id = parse_batch(message.payload, args.point, args.sample_rate_hz)
            try:
                batches.put_nowait((batch, event_id))
            except queue.Full:
                try:
                    batches.get_nowait()
                except queue.Empty:
                    pass
                try:
                    batches.put_nowait((batch, event_id))
                except queue.Full:
                    state.queue_drops += 1
        except (ValueError, UnicodeError, OSError) as exc:
            state.bad_messages += 1
            print(f"忽略无效 MQTT ADC event: {exc}", file=sys.stderr)

    def on_disconnect(_client: Any, _userdata: Any, *callback_args: Any) -> None:
        # Paho 1.x calls this callback with (client, userdata, rc), while
        # Paho 2.x adds disconnect flags and properties.
        reason_code = callback_args[-2] if len(callback_args) >= 2 else (
            callback_args[0] if callback_args else 0
        )
        if not reason_is_success(reason_code):
            print(f"MQTT 已断开: {reason_code}", file=sys.stderr)

    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect
    return client


def run_plot(args: argparse.Namespace) -> int:
    batches: queue.Queue[tuple[Batch, str]] = queue.Queue(maxsize=args.queue_size)
    state = WaveformState(args.window_seconds)
    client = make_mqtt_client(args, batches, state)
    try:
        client.connect(args.host, args.port, keepalive=args.keepalive)
        client.loop_start()
    except Exception as exc:  # paho uses several exception types across versions
        print(f"MQTT 连接失败: {exc}", file=sys.stderr)
        return 2

    figure, axis = plt.subplots()
    line = axis.plot([], [], color="#1769aa", linewidth=1.0)[0]
    status = axis.text(
        0.01, 0.99, "Waiting for MQTT ADC data...", transform=axis.transAxes,
        ha="left", va="top", fontsize=9,
    )
    axis.set_title(f"miniGateway ADC - {args.topic}")
    axis.set_xlabel("Time relative to latest sample (s)")
    axis.set_ylabel("Voltage (V)" if args.unit == "voltage" else "ADC value")
    axis.set_xlim(-args.window_seconds, 0.0)
    axis.grid(True, alpha=0.3)
    if args.ymin is not None and args.ymax is not None:
        axis.set_ylim(args.ymin, args.ymax)

    def update(_frame: int):
        received = 0
        while True:
            try:
                batch, event_id = batches.get_nowait()
            except queue.Empty:
                break
            state.add(batch, event_id)
            received += 1
        if received and state.timestamps_ns.size:
            x_values = (state.timestamps_ns - state.timestamps_ns[-1]) / 1_000_000_000.0
            line.set_data(x_values, state.values)
        if state.timestamps_ns.size:
            status.set_text(
                f"messages={state.message_count} samples={state.sample_count} "
                f"rate≈{state.sample_rate_hz:.1f} Hz\n"
                f"bad={state.bad_messages} queue_drops={state.queue_drops}"
            )
        return line, status

    animation = FuncAnimation(
        figure, update, interval=args.refresh_ms, blit=False, cache_frame_data=False
    )
    try:
        print("关闭图像窗口或按 Ctrl+C 退出。")
        plt.show()
    except KeyboardInterrupt:
        return 130
    finally:
        client.disconnect()
        client.loop_stop()
        _ = animation
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="MQTT broker 地址")
    parser.add_argument("--port", type=positive_int, default=1883)
    parser.add_argument("--topic", default="edge/events/adc-pico-1/event")
    parser.add_argument("--point", default="voltage")
    parser.add_argument("--client-id", default="adc-waveform-viewer")
    parser.add_argument("--username", default="")
    parser.add_argument("--password", default="")
    parser.add_argument("--qos", type=int, choices=(0, 1, 2), default=0)
    parser.add_argument("--keepalive", type=positive_int, default=30)
    parser.add_argument("--sample-rate-hz", type=positive_float, default=1000.0)
    parser.add_argument("--window-seconds", type=positive_float, default=5.0)
    parser.add_argument("--refresh-ms", type=positive_int, default=50)
    parser.add_argument("--queue-size", type=positive_int, default=64)
    parser.add_argument("--unit", choices=("voltage", "raw"), default="voltage")
    parser.add_argument("--ymin", type=float)
    parser.add_argument("--ymax", type=float)
    return parser


def main() -> int:
    return run_plot(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: GPL-3.0-only
"""Headless helper. Qt and all user interaction are owned by the OBS plugin."""
from base64 import b64encode
from dataclasses import asdict, is_dataclass, replace
from io import BytesIO
import json
import logging
from pathlib import Path
from queue import Empty, Queue
import sys
from threading import Thread
from time import monotonic
from urllib.parse import quote

from startlive.core import app_state, constant
from startlive.core.config_conversion.livehime import convert_collection, discover_configs, read_config
from startlive.core.constant.pk import PkMode
from startlive.core.pk.identity import current_identity
from startlive.core.pk.media_buffer import MediaBuffer
from startlive.core.pk.video_frames import packet_frame
from startlive.core.pk.output_readiness import output_readiness
from startlive.core.pk.states.session import PkOptions
from startlive.core.obs.states import ObsOutputStatus
from startlive.core.sign import RiskCaptchaCodec, gen_dm_track
from startlive.core.workers import WorkerManager
from startlive.core.workers.base import CallbackPresenter
from startlive.core.workers.pk.obs_media import ObsMediaWorker
from startlive.PySide.interface_adapters.pk.session_controller import PkSessionController

# Protocol-only stdout. Accounts, URLs and upstream data never go into stderr logs.
logging.disable(logging.CRITICAL)


def serial(value):
    if is_dataclass(value):
        return asdict(value)
    if isinstance(value, Path):
        return str(value)
    raise TypeError(f"Unsupported response type {type(value).__name__}")


def emit(**data):
    print(json.dumps(data, default=serial, ensure_ascii=True, separators=(",", ":")), flush=True)


class Dispatcher:
    def __init__(self, inbox):
        self.inbox = inbox

    def post(self, fn, *args, **kwargs):
        self.inbox.put(lambda: fn(*args, **kwargs))


class View:
    def __init__(self):
        self.remote_buffer = MediaBuffer()
        self.publish_buffer = MediaBuffer()
        self.local_buffer = MediaBuffer()
        self.invitation = None

    def show_invitation(self, event):
        self.invitation = event
        emit(event="invitation", args=[str(event.initiator or event.uid or event.peer_search or "")])

    def dismiss_invitation(self):
        self.invitation = None
        emit(event="dismiss_invitation", args=[])

    def require_classic_audio_ready(self):
        if not self.publish_buffer.freshness()[1]:
            raise RuntimeError("OBS 音频采集尚未就绪")

    def prepare_classic_audio(self):
        self.require_classic_audio_ready()

    def stop_classic_audio(self):
        pass

    def __getattr__(self, name):
        if name in {
            "set_status", "set_error", "set_active", "set_connection_active", "set_round_info",
            "set_invitation_remaining", "set_invitation_waiting", "set_continuous_status",
            "set_publisher_available", "set_publisher_mute_state", "refresh_controls",
            "set_busy", "set_values", "clear_values",
        }:
            return lambda *args, **kw: emit(event=name, args=args, fields=kw)
        raise AttributeError(name)


class Backend:
    def __init__(self, inbox):
        self.manager = WorkerManager(Dispatcher(inbox))
        self.view = View()
        self.owner = None
        self.capture = None
        self.capture_future = None
        self.pending = None
        self.candidates = {}
        self.preferences = None
        self.next_video = 0
        self.closed_announced = False

    def configure(self, args):
        config = json.loads(Path(args["identity"]).read_text(encoding="utf-8"))
        constant.APP_KEY = config["ak"]
        constant.APP_SECRET = config["as"]
        constant.LIVEHIME_BUILD = config["b"]
        constant.LIVEHIME_VERSION = config["v"]
        constant.HEADERS_WEB = config["hw"]
        constant.HEADERS_APP = config["ha"]

    def account(self, args):
        if self.owner and not self.owner.closed:
            raise ValueError("请先关闭当前 PK 会话")
        self.configure(args)
        app_state.replace_account_cookies(args["cookies"])
        app_state.room_info.update({"room_id": args["room"]})
        if args.get("device"):
            app_state.app_settings["app_buvid"] = args["device"]
        app_state.app_settings["proxy_mode"] = constant.ProxyMode(args.get("proxy_mode", 0))
        app_state.app_settings["custom_proxy_url"] = args.get("proxy", "")
        self.preferences = None
        self.candidates.clear()
        current_identity()

    def submit(self, request_id, worker, *args, transform=lambda result: result):
        presenter = CallbackPresenter(
            finished=lambda result: self._finish(request_id, lambda: transform(result)),
            failed=lambda error: emit(id=request_id, error=str(error)),
        )
        self.manager.submit(worker(presenter, current_identity(), *args))

    @staticmethod
    def _finish(request_id, fn):
        try:
            emit(id=request_id, value=fn())
        except Exception as error:
            emit(id=request_id, error=str(error))

    def _capture(self):
        if self.capture_future and not self.capture_future.done():
            return
        self.capture = ObsMediaWorker(CallbackPresenter(
            finished=lambda *args: None,
            failed=lambda error: self.view.set_status(str(error))),
            (self.view.local_buffer, self.view.publish_buffer), video=True, audio=True)
        self.capture_future = self.manager.submit(self.capture, on_progress=True)

    def _start(self, args):
        if self.owner and not self.owner.closed or self.pending:
            raise ValueError("当前已有 PK 会话")
        mode = PkMode(args["mode"])
        readiness = output_readiness(app_state.obs_output_status, mode)
        if not readiness.live_output_active:
            raise ValueError(readiness.reason)
        peer = int(args.get("peer") or 0) or None
        candidate = self.candidates.get(peer)
        if peer and not candidate:
            raise ValueError("请先搜索并选择对方")
        self._capture()
        self.pending = (monotonic() + 6, PkOptions(mode, peer, bool(args.get("continuous")), candidate),
                        bool(args.get("listen")))
        return True

    def _start_pending(self):
        deadline, options, listen = self.pending
        if self.owner and self.owner.manager.busy:
            if monotonic() >= deadline:
                self.pending = None
                emit(event="session_closed", args=[])
                self.view.set_status("上一个 PK 会话仍在关闭")
            return
        if options.mode == PkMode.TRADITIONAL and not self.view.publish_buffer.freshness()[1]:
            if monotonic() >= deadline:
                self.pending = None
                emit(event="session_closed", args=[])
                self.view.set_status("OBS 音频采集未就绪，无法启动经典 PK")
            return
        self.pending = None
        self.closed_announced = False
        self.owner = PkSessionController(
            self.manager, current_identity(), options, self.view,
            readiness=lambda mode: output_readiness(app_state.obs_output_status, mode),
            invitation_only=listen,
        )
        try:
            self.owner.start()
        except Exception:
            self.owner.close()
            raise

    def dispatch(self, request):
        request_id = request["id"]
        op, args = request["op"], request.get("args", {})
        if op == "ping":
            return emit(id=request_id, value={"version": 1})
        if op == "search":
            from startlive.core.workers.pk.search_candidates import PkSearchCandidatesWorker
            def found(page):
                self.candidates.update((item.uid, item) for item in page.users)
                return {"users": [{"uid": str(item.uid), "name": item.uname} for item in page.users],
                        "more": page.has_next}
            return self.submit(request_id, PkSearchCandidatesWorker, args["text"], transform=found)
        if op in ("pk_settings", "pk_settings_save"):
            from startlive.core.workers.pk.fetch_user_settings import FetchPkUserSettingsWorker
            from startlive.core.workers.pk.update_user_settings import UpdatePkUserSettingsWorker
            def loaded(result):
                self.preferences = getattr(result, "snapshot", result)
                return self.preferences
            if op == "pk_settings":
                return self.submit(request_id, FetchPkUserSettingsWorker, transform=loaded)
            if self.preferences is None:
                raise ValueError("请先读取 PK 设置")
            preferences = replace(self.preferences.preferences, **args["preferences"])
            if preferences.duration not in self.preferences.duration_options:
                raise ValueError("B 站未提供该 PK 时长")
            state = replace(self.preferences, preferences=preferences,
                            punish_time=int(args.get("punish_time", self.preferences.punish_time)))
            if state.punish_time not in state.punish_time_options:
                raise ValueError("B 站未提供该惩罚时长")
            return self.submit(request_id, UpdatePkUserSettingsWorker, state, transform=loaded)

        if op == "account":
            self.account(args)
        elif op == "output":
            app_state.obs_output_status = ObsOutputStatus(bool(args["active"]), bool(args["reconnecting"]), monotonic())
        elif op == "pk_start":
            self._start(args)
        elif op == "close":
            self.close()
        elif op == "face_qr":
            codec = RiskCaptchaCodec()
            content = codec.__risk_captcha_dec__(args["voucher"], args["content"])
            if content["type"] != "realname":
                raise ValueError("服务端要求了其他验证类型")
            token = quote(quote(codec.__risk_captcha_enc__({})["token"], safe=""), safe="")
            return emit(id=request_id, value={
                "url": "https://www.bilibili.com/h5/risk-control/realname?t=" + token,
                "token": content["token"], "track": gen_dm_track(),
            })
        elif op == "discover":
            root = args.get("path")
            return emit(id=request_id, value=discover_configs(Path(root) if root else None))
        elif op == "convert":
            path = Path(args["path"])
            result = convert_collection(read_config(path), name=args["name"],
                                        source_dir=path.parent,
                                        filter_prefixed_scenes=bool(args.get("filter")))
            return emit(id=request_id, value={"summary": result.summary(), "collection": result.collection})
        elif op == "crop":
            from startlive.core.pk.states.video_crop import VideoCrop
            self.view.publish_buffer.video_crop = VideoCrop(float(args["x"]), float(args["y"]))
        elif op in ("encoder", "encoder_scan"):
            from startlive.core.pk.states.encoder import VideoEncodingConfig
            if op == "encoder":
                config = VideoEncodingConfig.from_preferences(args)
                app_state.pk_encoder_settings.update(config=config)
            else:
                from startlive.core.workers.pk.scan_encoders import ScanPkEncodersWorker
                presenter = CallbackPresenter(
                    finished=lambda *values: emit(id=request_id, value=values),
                    failed=lambda error: emit(id=request_id, error=str(error)),
                )
                self.manager.submit(ScanPkEncodersWorker(presenter))
                return
        elif self.owner is None or self.owner.closed:
            raise ValueError("PK 会话尚未启动")
        elif op == "invitation":
            if self.view.invitation is not None:
                self.owner.answer_invitation(self.view.invitation, bool(args["accept"]))
        elif op in {"stop_round", "disconnect", "start_connection_pk", "set_continuous",
                    "set_publisher_muted", "set_remote_muted", "set_invitation_reception"}:
            getattr(self.owner, op)(*args.get("values", []))
        else:
            raise ValueError("未知操作")
        emit(id=request_id, value=True)

    def close(self):
        self.pending = None
        if self.owner:
            self.owner.close()
        if self.capture:
            self.capture.stop()
        self.view.remote_buffer.clear()

    def tick(self):
        if self.pending:
            self._start_pending()
        if self.owner:
            self.owner.tick()
            if self.owner.closed and not self.closed_announced:
                self.closed_announced = True
                emit(event="session_closed", args=[])
        current = monotonic()
        if current >= self.next_video:
            self.next_video = current + 1/12
            for kind, buffer in (("local_video", self.view.local_buffer), ("peer_video", self.view.remote_buffer)):
                if packet := buffer.take(1):
                    image = packet_frame(packet).to_image()
                    image.thumbnail((640, 480))
                    output = BytesIO()
                    image.save(output, "JPEG", quality=75)
                    emit(event=kind, args=[b64encode(output.getvalue()).decode()])
        for _ in range(8):
            packet = self.view.remote_buffer.take(2)
            if packet is None:
                break
            emit(event="peer_audio", args=[b64encode(packet.payload).decode(), packet.sample_rate, packet.frames])


def input_reader(inbox):
    for line in sys.stdin.buffer:
        if len(line) <= 2_000_000:
            try:
                inbox.put(json.loads(line))
            except ValueError:
                pass
    inbox.put(None)


def main():
    inbox = Queue()
    Thread(target=input_reader, args=(inbox,), daemon=True).start()
    backend = Backend(inbox)
    while True:
        try:
            item = inbox.get(timeout=0.02)
            if item is None:
                break
            if callable(item):
                item()
            elif isinstance(item, dict) and type(item.get("id")) is int:
                try:
                    backend.dispatch(item)
                except Exception as error:
                    emit(id=item["id"], error=str(error))
        except Empty:
            pass
        except Exception as error:
            emit(event="set_status", args=[str(error)])
        try:
            backend.tick()
        except Exception as error:
            backend.close()
            emit(event="session_closed", args=[])
            emit(event="set_status", args=[str(error)])
    backend.close()
    backend.manager.shutdown()


if __name__ == "__main__":
    main()

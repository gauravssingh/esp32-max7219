#!/usr/bin/env python3
"""
Herdr Matrix Display Notifier Hook
Streams agent status events from Herdr and pushes Done / Attention notifications
to the MAX7219 Dot Matrix Display at http://matrix-display.local/

Features:
- Event-driven priority queue for concurrent agents across workspaces
- Automatic network self-healing & subnet auto-discovery (< 1s)
- Hardware-autonomous clock revert via revert_ms
- Distinct status animations: opening (done), scroll_down (attention), wipe (working), blinds (error)
- Wireless Web OTA firmware flasher subcommand
- Rich Herdr workspace project tag resolution
"""

import collections
import heapq
import json
import logging
import os
import signal
import socket
import sys
import threading
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# --- Configuration Paths ---
HOME = Path.home()
CONFIG_DIR = Path(os.environ.get("HERDR_PLUGIN_CONFIG_DIR", HOME / ".config" / "herdr-matrix-notifier"))
STATE_DIR = Path(os.environ.get("HERDR_PLUGIN_STATE_DIR", HOME / ".local" / "state" / "herdr-matrix-notifier"))
PID_FILE = STATE_DIR / "notifier.pid"
LOG_FILE = STATE_DIR / "notifier.log"
LAST_IP_FILE = STATE_DIR / "last_known_ip.txt"
CONFIG_FILE = CONFIG_DIR / "config.json"

DEFAULT_SOCKET_PATH = str(HOME / ".config" / "herdr" / "herdr.sock")
DEFAULT_CONFIG = {
    "matrix_url": "http://matrix-display.local/",
    "fallback_url": "http://192.168.68.73/",
    "align": "center",
    "scroll_speed": 22,
    "pause_ms": 3000,
    # Distinct animations per status
    "effect_done": "opening",         # "from center out" - opens outward from center
    "effect_blocked": "scroll_down",  # "slide from top" - drops down from above
    "effect_attention": "scroll_down",# alias for blocked
    "effect_working": "wipe",         # "from the right" - clean horizontal wipe reveal
    "effect_error": "blinds",         # vertical blinds warning reveal
    # Tag display mode: "agent" ([v]AGY), "workspace" ([v]S3), "both" ([v]AGY:S3)
    "tag_mode": "agent",
    "include_workspace": False,
    # Templates per status
    "done_template": "[v]{tag}",
    "blocked_template": "! {tag} !",
    "attention_template": "! {tag} !",
    "working_template": "> {tag}",
    "error_template": "X {tag}",
    # Notification flags
    "notify_on_working": False,       # keep clock visible; only alert on done / blocked
    "notify_on_error": True,
    "use_hardware_clock": False,      # False pushes clean HH:MM; True hands over to NTP RTC
    "clock_revert_seconds": 25,       # revert to digital clock after 25s
    "cooldown_seconds": 3.0,
    "quiet_agents": []
}

KNOWN_PROJECT_TAGS = {
    "esp32-s3": "S3",
    "esp32-max7219": "MAX",
    "tsadmin": "TS",
    "dexlab": "DEX",
    "proptally-mt": "PROP",
    "dash": "DASH",
    "bambu-monitor": "3D",
    "my-monee": "MON",
    "hermes": "HERM",
    "electro": "ELEC",
    "os-tune-up": "TUNE"
}

def ensure_dirs():
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    STATE_DIR.mkdir(parents=True, exist_ok=True)

def load_config():
    cfg = dict(DEFAULT_CONFIG)
    if CONFIG_FILE.exists():
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                cfg.update(json.load(f))
        except Exception as e:
            logging.warning(f"Failed to read config file {CONFIG_FILE}: {e}")
    return cfg

def setup_logging(foreground=False):
    ensure_dirs()
    handlers = [logging.FileHandler(LOG_FILE, encoding="utf-8")]
    if foreground:
        handlers.append(logging.StreamHandler(sys.stdout))
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=handlers
    )

# --- Dynamic IP Resolution & Subnet Auto-Discovery ---
_IP_CACHE = {}
_IP_CACHE_TTL = 300  # 5 minutes

def get_last_known_ip():
    if LAST_IP_FILE.exists():
        try:
            ip = LAST_IP_FILE.read_text().strip()
            if ip: return ip
        except Exception:
            pass
    return "192.168.68.73"

def save_last_known_ip(ip):
    try:
        ensure_dirs()
        LAST_IP_FILE.write_text(ip)
    except Exception:
        pass

def discover_matrix_ip():
    """Ultra-fast concurrent probe across the local subnet to find the MAX7219 ESP32."""
    def probe(ip):
        try:
            url = f"http://{ip}/api/status"
            req = urllib.request.Request(url, headers={"User-Agent": "matrix-notifier"})
            with urllib.request.urlopen(req, timeout=0.35) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                if "dots" in data and "effect" in data:
                    return ip
        except Exception:
            pass
        return None

    # Probe common DHCP range 50..100 first
    first_pass = [f"192.168.68.{i}" for i in range(50, 100)]
    with ThreadPoolExecutor(max_workers=30) as ex:
        for res in ex.map(probe, first_pass):
            if res:
                logging.info(f"Auto-discovered matrix display at {res}")
                save_last_known_ip(res)
                return res

    # Wider /22 probe if needed
    wider_pass = [f"192.168.{s}.{i}" for s in [68, 69, 70, 71] for i in range(1, 255)]
    with ThreadPoolExecutor(max_workers=50) as ex:
        for res in ex.map(probe, wider_pass):
            if res:
                logging.info(f"Auto-discovered matrix display on wide scan at {res}")
                save_last_known_ip(res)
                return res

    return None

def resolve_host_ipv4(host):
    now = time.time()
    if host in _IP_CACHE:
        ip, expire = _IP_CACHE[host]
        if now < expire:
            return ip
    try:
        ip = socket.gethostbyname(host)
        _IP_CACHE[host] = (ip, now + _IP_CACHE_TTL)
        save_last_known_ip(ip)
        return ip
    except Exception:
        cached = get_last_known_ip()
        if cached:
            return cached
        return host

# --- Matrix Display HTTP Client ---
def push_to_matrix(config, text, effect=None, align=None, revert_ms=None):
    matrix_url = config.get("matrix_url", "http://matrix-display.local/").rstrip("/")
    fallback_url = config.get("fallback_url", "http://192.168.68.73/").rstrip("/")
    speed = config.get("scroll_speed", 22)
    pause = config.get("pause_ms", 3000)
    eff = effect or config.get("effect_done", "opening")
    aln = align or config.get("align", "center")
    revert_delay = revert_ms if revert_ms is not None else (config.get("clock_revert_seconds", 25) * 1000)

    params = {
        "text": text,
        "effect": eff,
        "align": aln,
        "speed": speed,
        "pause": pause,
        "save": 0
    }
    if revert_delay > 0:
        params["revert_ms"] = revert_delay

    data = urllib.parse.urlencode(params).encode("utf-8")

    urls_to_try = [matrix_url]
    cached_ip = get_last_known_ip()
    if cached_ip:
        cand = f"http://{cached_ip}/"
        if cand not in urls_to_try:
            urls_to_try.append(cand)
    if fallback_url and fallback_url not in urls_to_try:
        urls_to_try.append(fallback_url)

    for base in urls_to_try:
        try:
            parsed = urllib.parse.urlparse(base)
            host = parsed.hostname or "matrix-display.local"
            port = parsed.port
            fast_ip = resolve_host_ipv4(host)

            netloc = f"{fast_ip}:{port}" if port else fast_ip
            target_url = urllib.parse.urlunparse((parsed.scheme, netloc, "/api/message", "", "", ""))

            req = urllib.request.Request(
                target_url,
                data=data,
                headers={"Host": host},
                method="POST"
            )
            with urllib.request.urlopen(req, timeout=2.5) as resp:
                if resp.status == 200:
                    logging.info(f"Pushed to matrix ({host} -> {fast_ip}): '{text}' [{eff}/{aln}]")
                    save_last_known_ip(fast_ip)
                    return True
        except Exception as e:
            logging.debug(f"Push to {base} failed: {e}")
            continue

    # Attempt automatic discovery if all cached endpoints failed
    logging.warning("Configured URLs unreachable, attempting subnet auto-discovery...")
    discovered = discover_matrix_ip()
    if discovered:
        try:
            target_url = f"http://{discovered}/api/message"
            req = urllib.request.Request(target_url, data=data, headers={"Host": discovered}, method="POST")
            with urllib.request.urlopen(req, timeout=2.5) as resp:
                if resp.status == 200:
                    logging.info(f"Pushed via auto-discovery to {discovered}: '{text}' [{eff}/{aln}]")
                    return True
        except Exception as e:
            logging.error(f"Auto-discovery push failed: {e}")

    logging.error(f"Failed to push message '{text}' to matrix display (tried {urls_to_try})")
    return False

def revert_to_clock(config):
    if config.get("use_hardware_clock", False):
        # Trigger ESP32's internal NTP hardware clock
        matrix_url = config.get("matrix_url", "http://matrix-display.local/").rstrip("/")
        fallback_url = config.get("fallback_url", "http://192.168.68.73/").rstrip("/")
        data = urllib.parse.urlencode({"clock": 1, "save": 0}).encode("utf-8")
        urls_to_try = [matrix_url]
        cached_ip = get_last_known_ip()
        if cached_ip:
            cand = f"http://{cached_ip}/"
            if cand not in urls_to_try:
                urls_to_try.append(cand)
        if fallback_url and fallback_url not in urls_to_try:
            urls_to_try.append(fallback_url)

        for base in urls_to_try:
            try:
                parsed = urllib.parse.urlparse(base)
                host = parsed.hostname or "matrix-display.local"
                port = parsed.port
                fast_ip = resolve_host_ipv4(host)
                netloc = f"{fast_ip}:{port}" if port else fast_ip
                target_url = urllib.parse.urlunparse((parsed.scheme, netloc, "/api/message", "", "", ""))
                req = urllib.request.Request(target_url, data=data, headers={"Host": host}, method="POST")
                with urllib.request.urlopen(req, timeout=2.5) as resp:
                    if resp.status == 200:
                        logging.info(f"Restored ESP32 hardware clock ({fast_ip})")
                        save_last_known_ip(fast_ip)
                        return True
            except Exception:
                continue
        return False
    else:
        now_str = time.strftime("%H:%M")
        return push_to_matrix(config, now_str, effect="print", align="center")

def set_matrix_brightness(config, level):
    level = max(0, min(15, int(level)))
    data = urllib.parse.urlencode({"brightness": level}).encode("utf-8")
    target_ip = get_last_known_ip() or "192.168.68.73"
    req = urllib.request.Request(f"http://{target_ip}/api/brightness", data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=2.5) as resp:
            return resp.status == 200
    except Exception as e:
        logging.error(f"Set brightness failed: {e}")
        return False

# --- Web OTA Firmware Updater ---
def push_ota_firmware(bin_path, target_ip=None):
    ip = target_ip or get_last_known_ip() or "192.168.68.73"
    p = Path(bin_path).resolve()
    if not p.exists():
        print(f"Error: Firmware binary not found at {p}")
        return False

    file_bytes = p.read_bytes()
    print(f"Uploading {len(file_bytes)} bytes to http://{ip}/update over Wi-Fi...")

    boundary = "----MatrixOtaBoundary7MA4YWxkTrZu0gW"
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; filename="{p.name}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode("utf-8") + file_bytes + f"\r\n--{boundary}--\r\n".encode("utf-8")

    req = urllib.request.Request(
        f"http://{ip}/update",
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            res_text = resp.read().decode("utf-8", errors="replace")
            print(f"OTA Result: HTTP {resp.status} - {res_text}")
            return resp.status == 200
    except Exception as e:
        print(f"OTA Flash failed: {e}")
        return False

# --- Herdr Socket Client ---
def get_socket_path():
    return os.environ.get("HERDR_SOCKET_PATH") or DEFAULT_SOCKET_PATH

def request_rpc(method, params=None, timeout=5.0):
    """Single RPC call to Herdr."""
    sock_path = get_socket_path()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    req = {"id": f"rpc:{int(time.time()*1000)}", "method": method, "params": params or {}}
    s.sendall((json.dumps(req) + "\n").encode("utf-8"))

    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    s.close()

    if not buf:
        return None
    try:
        return json.loads(buf.split(b"\n")[0].decode("utf-8"))
    except Exception as e:
        logging.error(f"RPC decode error for {method}: {e}")
        return None

# --- Main Event Stream Daemon with Priority Queue ---
class NotifierDaemon:
    def __init__(self, config):
        self.config = config
        self.running = True
        self.panes_state = {}     # pane_id -> {status, agent, workspace, last_seen}
        self.workspaces_map = {}  # workspace_id -> short project tag (e.g. S3, DEX, TS)
        self.last_alert_text = None
        self.last_alert_time = 0
        self.clock_revert_time = 0
        self.last_clock_minute = None

        # Priority queue: items are tuples (priority, timestamp, alert_text, effect)
        # Priority 0 = Blocked/Attention (Highest)
        # Priority 1 = Done
        # Priority 2 = Working/Info
        self.alert_queue = []
        self.queue_lock = threading.Lock()

    def get_snapshot_panes(self):
        resp = request_rpc("session.snapshot")
        if not resp or "result" not in resp:
            return []
        snap = resp.get("result", {}).get("snapshot", {})
        workspaces = snap.get("workspaces", [])
        for w in workspaces:
            wid = w.get("workspace_id")
            lbl = w.get("label", "")
            if wid:
                self.workspaces_map[wid] = self.format_workspace_tag(lbl, wid)
        return snap.get("panes", [])

    @staticmethod
    def format_workspace_tag(label, workspace_id=""):
        if not label:
            return workspace_id or ""
        lbl = label.lower().strip()
        if lbl in KNOWN_PROJECT_TAGS:
            return KNOWN_PROJECT_TAGS[lbl]
        if "-" in lbl:
            parts = lbl.split("-")
            for p in reversed(parts):
                if p in ("s3", "esp32", "mt", "app", "cli", "dev", "ui"):
                    return p.upper()
            return parts[0][:4].upper()
        if len(lbl) <= 4:
            return lbl.upper()
        return lbl[:4].upper()

    def format_agent_name(self, agent_raw, display_agent=None):
        if display_agent:
            return display_agent.upper()
        if agent_raw:
            mapping = {
                "agy": "AGY",
                "antigravity": "AGY",
                "claude": "CLAUDE",
                "cursor": "CURSOR",
                "codex": "CODEX",
                "copilot": "COPILOT",
                "pi": "PI",
                "devin": "DEVIN",
                "hermes": "HERMES"
            }
            return mapping.get(agent_raw.lower(), agent_raw.upper())
        return "AGENT"

    def enqueue_alert(self, priority, text, effect):
        now = time.time()
        cooldown = self.config.get("cooldown_seconds", 3.0)
        if text == self.last_alert_text and (now - self.last_alert_time) < cooldown:
            logging.debug(f"Suppressed duplicate alert within cooldown: '{text}'")
            return

        with self.queue_lock:
            heapq.heappush(self.alert_queue, (priority, now, text, effect))
            logging.info(f"Enqueued alert (priority {priority}): '{text}' [{effect}] (queue depth: {len(self.alert_queue)})")

    def process_queue_tick(self):
        now = time.time()
        # If an alert is currently holding on screen, respect its dwell window
        if now < self.clock_revert_time:
            return

        with self.queue_lock:
            if self.alert_queue:
                priority, ts, alert_text, effect = heapq.heappop(self.alert_queue)
                self.last_alert_text = alert_text
                self.last_alert_time = now
                pause_s = self.config.get("pause_ms", 3000) / 1000.0
                revert_delay = self.config.get("clock_revert_seconds", 25)
                # Next alert or clock revert timestamp
                self.clock_revert_time = now + max(pause_s, 3.5) if self.alert_queue else now + revert_delay

                push_to_matrix(
                    self.config,
                    alert_text,
                    effect=effect,
                    align=self.config.get("align", "center"),
                    revert_ms=int(revert_delay * 1000)
                )
                return

        # Queue is empty and alert dwell elapsed -> revert to clock
        if self.clock_revert_time > 0 and now >= self.clock_revert_time:
            self.clock_revert_time = 0
            revert_to_clock(self.config)
            self.last_clock_minute = time.strftime("%H:%M")
        elif not self.config.get("use_hardware_clock", False) and self.clock_revert_time == 0:
            cur_min = time.strftime("%H:%M")
            if cur_min != self.last_clock_minute:
                revert_to_clock(self.config)
                self.last_clock_minute = cur_min

    def handle_status_change(self, pane_id, new_status, agent_raw, display_agent, workspace_id, title=None):
        now = time.time()
        agent_name = self.format_agent_name(agent_raw, display_agent)
        ws_tag = self.workspaces_map.get(workspace_id, workspace_id or "")

        quiet = self.config.get("quiet_agents", [])
        if agent_raw and agent_raw.lower() in [q.lower() for q in quiet]:
            return

        old_info = self.panes_state.get(pane_id, {})
        old_status = old_info.get("status")

        # Update local memory
        self.panes_state[pane_id] = {
            "status": new_status,
            "agent": agent_name,
            "workspace": ws_tag,
            "last_seen": now
        }

        # Skip if no real status change
        if old_status == new_status:
            return

        logging.info(f"Pane {pane_id} ({agent_name} in {ws_tag or 'default'}): status {old_status} -> {new_status}")

        # Determine tag text based on tag_mode
        tag_mode = self.config.get("tag_mode", "agent")
        if tag_mode == "workspace" and ws_tag:
            tag = ws_tag
        elif tag_mode == "both" and ws_tag:
            tag = f"{agent_name}:{ws_tag}"
        else:
            tag = agent_name

        # Priority 0: Agent Needs Attention (Blocked)
        if new_status == "blocked":
            tpl = self.config.get("blocked_template", self.config.get("attention_template", "! {tag} !"))
            text = tpl.format(tag=tag, agent=agent_name, workspace=ws_tag).strip()
            eff = self.config.get("effect_blocked", self.config.get("effect_attention", "scroll_down"))
            self.enqueue_alert(0, text, eff)

        # Priority 1: Agent Done
        elif new_status == "done" or (old_status == "working" and new_status in ("done", "idle")):
            tpl = self.config.get("done_template", "[v]{tag}")
            text = tpl.format(tag=tag, agent=agent_name, workspace=ws_tag).strip()
            eff = self.config.get("effect_done", "opening")
            self.enqueue_alert(1, text, eff)

        # Priority 2: Agent Error / Failed
        elif new_status in ("error", "failed"):
            if self.config.get("notify_on_error", True):
                tpl = self.config.get("error_template", "X {tag}")
                text = tpl.format(tag=tag, agent=agent_name, workspace=ws_tag).strip()
                eff = self.config.get("effect_error", "blinds")
                self.enqueue_alert(2, text, eff)

        # Priority 3: Agent Started Working
        elif new_status == "working":
            if self.config.get("notify_on_working", False):
                tpl = self.config.get("working_template", "> {tag}")
                text = tpl.format(tag=tag, agent=agent_name, workspace=ws_tag).strip()
                eff = self.config.get("effect_working", "wipe")
                self.enqueue_alert(3, text, eff)

    def run(self):
        logging.info("Starting Herdr Matrix Notifier event stream listener...")

        while self.running:
            sock_path = get_socket_path()
            if not os.path.exists(sock_path):
                logging.warning(f"Herdr socket {sock_path} not found. Waiting...")
                time.sleep(3)
                continue

            # Step 1: Baseline snapshot
            try:
                initial_panes = self.get_snapshot_panes()
                for p in initial_panes:
                    pid = p.get("pane_id")
                    st = p.get("agent_status")
                    ag = p.get("agent")
                    ws = p.get("workspace_id")
                    if pid and st:
                        self.panes_state[pid] = {
                            "status": st,
                            "agent": self.format_agent_name(ag, p.get("display_agent")),
                            "workspace": self.workspaces_map.get(ws, ws or "default"),
                            "last_seen": time.time()
                        }
                logging.info(f"Loaded baseline snapshot: {len(initial_panes)} panes across {len(self.workspaces_map)} workspaces")
                if self.clock_revert_time == 0:
                    revert_to_clock(self.config)
                    self.last_clock_minute = time.strftime("%H:%M")
            except Exception as e:
                logging.error(f"Snapshot baseline failed: {e}")
                time.sleep(2)
                continue

            # Step 2: Open long-lived connection for events.subscribe
            sub_sock = None
            try:
                sub_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sub_sock.connect(sock_path)

                subs = [
                    {"type": "pane.created"},
                    {"type": "pane.closed"},
                    {"type": "pane.exited"},
                    {"type": "pane.focused"}
                ]
                for pid in self.panes_state.keys():
                    subs.append({"type": "pane.agent_status_changed", "pane_id": pid})

                req = {
                    "id": f"sub:{int(time.time())}",
                    "method": "events.subscribe",
                    "params": {"subscriptions": subs}
                }
                sub_sock.sendall((json.dumps(req) + "\n").encode("utf-8"))
                sub_sock.settimeout(0.5)

                line_buf = ""
                reconcile_needed = False

                while self.running:
                    # Tick alert queue & clock updates
                    self.process_queue_tick()

                    try:
                        chunk = sub_sock.recv(4096)
                        if not chunk:
                            logging.warning("Event stream connection closed by Herdr server")
                            break
                    except socket.timeout:
                        continue

                    line_buf += chunk.decode("utf-8", errors="replace")
                    while "\n" in line_buf:
                        line, line_buf = line_buf.split("\n", 1)
                        line = line.strip()
                        if not line:
                            continue

                        try:
                            msg = json.loads(line)
                        except Exception:
                            continue

                        if msg.get("result", {}).get("type") == "subscription_started":
                            logging.info("Herdr event subscription active")
                            continue

                        event_type = msg.get("event")
                        data = msg.get("data", {})

                        if event_type == "pane.agent_status_changed":
                            pid = data.get("pane_id")
                            status = data.get("agent_status")
                            agent = data.get("agent")
                            display_agent = data.get("display_agent")
                            workspace = data.get("workspace_id")
                            title = data.get("title")
                            if pid and status:
                                self.handle_status_change(pid, status, agent, display_agent, workspace, title)

                        elif event_type in ("pane.created", "pane.closed", "pane.exited", "pane_created", "pane_closed", "pane_exited"):
                            reconcile_needed = True

                    if reconcile_needed:
                        logging.info("Pane topology changed, refreshing subscription set...")
                        break

            except Exception as e:
                logging.error(f"Event subscription loop error: {e}")
            finally:
                if sub_sock:
                    try:
                        sub_sock.close()
                    except Exception:
                        pass

            if self.running:
                time.sleep(1)

        logging.info("Notifier event stream stopped.")

# --- Process Lifecycle & CLI Subcommands ---
def get_running_pid():
    if not PID_FILE.exists():
        return None
    try:
        pid = int(PID_FILE.read_text().strip())
        os.kill(pid, 0)
        return pid
    except (ValueError, ProcessLookupError, PermissionError):
        PID_FILE.unlink(missing_ok=True)
        return None

def cmd_start():
    ensure_dirs()
    pid = get_running_pid()
    if pid:
        print(f"Matrix Notifier already running (pid {pid})")
        return 0

    script_path = Path(__file__).resolve()
    proc = sys.executable
    if sys.platform == "win32":
        import subprocess
        proc = subprocess.Popen([sys.executable, str(script_path), "run"], creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        import subprocess
        proc = subprocess.Popen(
            [sys.executable, str(script_path), "run"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            start_new_session=True
        )
    PID_FILE.write_text(str(proc.pid))
    print(f"Matrix Notifier started (pid {proc.pid})")
    print(f"Logs: {LOG_FILE}")
    return 0

def cmd_stop():
    pid = get_running_pid()
    if not pid:
        print("Matrix Notifier is not running")
        return 0
    try:
        os.kill(pid, signal.SIGTERM)
        for _ in range(30):
            time.sleep(0.1)
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
        else:
            os.kill(pid, signal.SIGKILL)
        PID_FILE.unlink(missing_ok=True)
        print(f"Matrix Notifier stopped (pid {pid})")
    except Exception as e:
        print(f"Error stopping process {pid}: {e}")
        return 1
    return 0

def cmd_restart():
    cmd_stop()
    time.sleep(0.5)
    return cmd_start()

def cmd_status():
    pid = get_running_pid()
    config = load_config()
    print("=== Herdr Matrix Display Notifier ===")
    print(f"Status:       {'RUNNING (pid ' + str(pid) + ')' if pid else 'STOPPED'}")
    print(f"Herdr Socket: {get_socket_path()} ({'exists' if os.path.exists(get_socket_path()) else 'NOT FOUND'})")
    print(f"Matrix URL:   {config.get('matrix_url')}")
    print(f"Last Good IP: {get_last_known_ip()}")
    print(f"Logs:         {LOG_FILE}")

    # Test Matrix HTTP
    try:
        target_ip = get_last_known_ip() or "192.168.68.73"
        req = urllib.request.Request(f"http://{target_ip}/api/status")
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            print(f"Matrix Link:  ONLINE (HTTP {resp.status} - Text: '{data.get('text', '')}')")
    except Exception as e:
        print(f"Matrix Link:  OFFLINE or SLOW ({e})")
    return 0

def cmd_clock():
    config = load_config()
    print("Reverting display to clock mode...")
    ok = revert_to_clock(config)
    print(f"Result: {'SUCCESS' if ok else 'FAILED'}")
    return 0 if ok else 1

def cmd_brightness(val):
    config = load_config()
    level = int(val)
    print(f"Setting matrix brightness to {level} (0-15)...")
    ok = set_matrix_brightness(config, level)
    print(f"Result: {'SUCCESS' if ok else 'FAILED'}")
    return 0 if ok else 1

def cmd_test_done():
    config = load_config()
    msg = "[v]AGY"
    eff = config.get("effect_done", "opening")
    aln = config.get("align", "center")
    print(f"Sending test done alert ({eff}/{aln}): '{msg}'")
    ok = push_to_matrix(config, msg, effect=eff, align=aln)
    print(f"Result: {'SUCCESS' if ok else 'FAILED'}")
    return 0 if ok else 1

def cmd_test_blocked():
    config = load_config()
    msg = "! AGY !"
    eff = config.get("effect_blocked", config.get("effect_attention", "scroll_down"))
    aln = config.get("align", "center")
    print(f"Sending test attention alert ({eff}/{aln}): '{msg}'")
    ok = push_to_matrix(config, msg, effect=eff, align=aln)
    print(f"Result: {'SUCCESS' if ok else 'FAILED'}")
    return 0 if ok else 1

def cmd_test_working():
    config = load_config()
    msg = "> AGY"
    eff = config.get("effect_working", "wipe")
    aln = config.get("align", "center")
    print(f"Sending test working alert ({eff}/{aln}): '{msg}'")
    ok = push_to_matrix(config, msg, effect=eff, align=aln)
    print(f"Result: {'SUCCESS' if ok else 'FAILED'}")
    return 0 if ok else 1

def cmd_test_error():
    config = load_config()
    msg = "X AGY"
    eff = config.get("effect_error", "blinds")
    aln = config.get("align", "center")
    print(f"Sending test error alert ({eff}/{aln}): '{msg}'")
    ok = push_to_matrix(config, msg, effect=eff, align=aln)
    print(f"Result: {'SUCCESS' if ok else 'FAILED'}")
    return 0 if ok else 1

def cmd_ota(bin_path=None):
    if not bin_path:
        default_p = Path(__file__).resolve().parent.parent / ".pio" / "build" / "esp32dev" / "firmware.bin"
        if default_p.exists():
            bin_path = str(default_p)
        else:
            print("Usage: python3 notifier.py ota <path_to_firmware.bin>")
            return 1
    ok = push_ota_firmware(bin_path)
    return 0 if ok else 1

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"

    if cmd == "start":
        sys.exit(cmd_start())
    elif cmd == "stop":
        sys.exit(cmd_stop())
    elif cmd == "restart":
        sys.exit(cmd_restart())
    elif cmd == "status":
        sys.exit(cmd_status())
    elif cmd == "clock":
        sys.exit(cmd_clock())
    elif cmd == "brightness":
        val = sys.argv[2] if len(sys.argv) > 2 else "7"
        sys.exit(cmd_brightness(val))
    elif cmd == "dim":
        sys.exit(cmd_brightness(1))
    elif cmd == "bright":
        sys.exit(cmd_brightness(10))
    elif cmd == "test-done":
        sys.exit(cmd_test_done())
    elif cmd == "test-blocked":
        sys.exit(cmd_test_blocked())
    elif cmd == "test-working":
        sys.exit(cmd_test_working())
    elif cmd == "test-error":
        sys.exit(cmd_test_error())
    elif cmd == "ota":
        bin_p = sys.argv[2] if len(sys.argv) > 2 else None
        sys.exit(cmd_ota(bin_p))
    elif cmd == "run":
        setup_logging(foreground=False)
        config = load_config()
        daemon = NotifierDaemon(config)

        def sig_handler(signum, frame):
            daemon.running = False

        signal.signal(signal.SIGTERM, sig_handler)
        signal.signal(signal.SIGINT, sig_handler)
        daemon.run()
    else:
        print("Usage: python3 notifier.py <start|stop|restart|status|clock|dim|bright|test-done|test-blocked|test-working|test-error|ota>")
        sys.exit(2)

if __name__ == "__main__":
    main()

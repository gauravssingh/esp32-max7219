# Herdr Matrix Display Notifier (`gs.matrix-notifier`)

A Herdr workflow plugin that automatically pushes **Agent Done** and **Agent Needs Attention** alerts from all Herdr panes to your physical **MAX7219 Dot Matrix Display** (`http://matrix-display.local/`).

---

## How It Works

1. **Background Socket Event Listener**: Connects to the local Herdr Unix domain socket (`~/.config/herdr/herdr.sock`) and subscribes to live pane and agent lifecycle events (`pane.agent_status_changed`).
2. **Done Alert**: When any agent (Antigravity, Claude, Cursor, Codex, Copilot, Pi, etc.) finishes its task, the plugin formats and pushes a notification to the matrix display:
   ```
   OK: ANTIGRAVITY DONE (wZ)
   ```
3. **Needs Attention Alert**: When an agent gets blocked on a confirmation prompt, interview question, or approval gate (`agent_status == "blocked"`), the plugin pushes an urgent scrolling alert:
   ```
   ! ATTN: CLAUDE WAITING (wA) !
   ```
4. **Auto-Recovery & Reconnect**: Automatically handles Herdr restarts and pane creation/closure. If mDNS resolution is delayed, it falls back to the direct local IP (`http://192.168.68.71/`).

---

## Installation & Linking

Link the plugin into your local Herdr runtime:

```bash
herdr plugin link ~/projects/herdr-plugins/matrix-notifier
```

Verify it is linked and enabled:

```bash
herdr plugin list
```

---

## CLI & Action Usage

The plugin provides lifecycle actions callable directly via the `herdr` CLI or terminal:

### Start the daemon:
```bash
herdr plugin action invoke gs.matrix-notifier.start
# or directly:
python3 ~/projects/herdr-plugins/matrix-notifier/notifier.py start
```

### Check status:
```bash
python3 ~/projects/herdr-plugins/matrix-notifier/notifier.py status
```

### Stop the daemon:
```bash
python3 ~/projects/herdr-plugins/matrix-notifier/notifier.py stop
```

### Send a test alert:
```bash
# Test Done Alert:
python3 ~/projects/herdr-plugins/matrix-notifier/notifier.py test-done

# Test Attention Alert:
python3 ~/projects/herdr-plugins/matrix-notifier/notifier.py test-blocked
```

---

## Configuration

Settings are saved in `~/.config/herdr-matrix-notifier/config.json`:

```json
{
  "matrix_url": "http://matrix-display.local/",
  "fallback_url": "http://192.168.68.71/",
  "scroll_speed": 32,
  "pause_ms": 1500,
  "done_template": "OK: {agent} DONE ({workspace})",
  "attention_template": "! ATTN: {agent} WAITING ({workspace}) !",
  "cooldown_seconds": 4.0,
  "quiet_agents": []
}
```

---

## Logs

Execution logs are written to:
```bash
tail -f ~/.local/state/herdr-matrix-notifier/notifier.log
```

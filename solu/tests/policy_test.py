"""Unit tests for when Solu asks first (actions.needs_approval / describe): python tests/policy_test.py
The user's rule: do everything at once, except deleting, writing, closing (and sending something as the user)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import actions  # noqa: E402

ASKS = [("write_file", {"path": "notes.txt", "content": "hi"}),
        ("send_email", {"to": "a@b.com", "subject": "s", "body": "b"}),
        ("tell_agent", {"agent": "claude", "message": "fix it"}),
        ("window_control", {"action": "close", "title": "Notepad"}),
        ("press_keys", {"keys": "alt+f4"}),
        ("press_keys", {"keys": "ctrl+w"}),
        ("press_keys", {"keys": "shift+delete"}),
        ("run_command", {"command": "Remove-Item C:\\temp\\x.txt"}),
        ("run_command", {"command": "Set-Content a.txt hello"}),
        ("run_command", {"command": "echo hi > a.txt"}),
        ("open_file", {"path": "C:\\Downloads\\setup.exe"})]
JUST_DOES = [("click_element", {"name": "Eusha Ibna Akbor"}), ("click_text", {"text": "Sign in"}),
             ("type_text", {"text": "hello"}), ("press_keys", {"keys": "enter"}), ("press_keys", {"keys": "ctrl+c"}),
             ("scroll", {"direction": "down", "pages": 0.3}), ("open_app", {"name": "Chrome"}),
             ("open_website", {"url": "youtube.com"}), ("play_song", {"query": "maths"}), ("media_control", {"action": "next"}),
             ("set_volume", {"percent": 30}), ("window_control", {"action": "maximize"}), ("find_files", {"name": "resume"}),
             ("read_file", {"path": "notes.txt"}), ("open_file", {"path": "C:\\Docs\\report.pdf"}),
             ("run_command", {"command": "Get-Date"}), ("clipboard", {"action": "read"}), ("start_task", {"goal": "x"})]

failed = 0
for name, args in ASKS:
    ok = actions.needs_approval(name, args)
    action, detail, _ = actions.describe(name, args)
    readable = not action.startswith("use ") and "{" not in detail and "_" not in action
    print(f"[{'PASS' if ok and readable else 'FAIL'}] asks first: {name} {args}  -> card: {action!r}")
    failed += not (ok and readable)
for name, args in JUST_DOES:
    ok = not actions.needs_approval(name, args)
    print(f"[{'PASS' if ok else 'FAIL'}] just does it: {name} {args}")
    failed += not ok
unknown = sorted({n for n, _ in ASKS + JUST_DOES} - actions.TOOL_NAMES)
if unknown:
    print(f"[FAIL] not real tools: {unknown}")
    failed += 1
print("\nall passed" if not failed else f"\n{failed} failed")
sys.exit(1 if failed else 0)

"""Eyes that understand controls: what the foreground window offers to click, type into or choose, from Windows' own UI
Automation tree (names, kinds, positions). Much faster and more exact than guessing from pixels; screen.py's OCR is the
fallback for windows that expose nothing (games, canvases, remote desktops).
"""
import ctypes
import time

import uiautomation as auto

user32 = ctypes.windll.user32
auto.SetGlobalSearchTimeout(1.0)

# What a person can act on. Everything else (panes, groups, plain text) is skipped.
INTERACTIVE = {"ButtonControl", "EditControl", "MenuItemControl", "ListItemControl", "TabItemControl", "CheckBoxControl",
               "RadioButtonControl", "HyperlinkControl", "ComboBoxControl", "TreeItemControl", "SplitButtonControl",
               "DataItemControl", "SliderControl"}
KIND = {"ButtonControl": "button", "EditControl": "text field", "MenuItemControl": "menu item", "ListItemControl": "list item",
        "TabItemControl": "tab", "CheckBoxControl": "checkbox", "RadioButtonControl": "radio", "HyperlinkControl": "link",
        "ComboBoxControl": "dropdown", "TreeItemControl": "tree item", "SplitButtonControl": "button", "DataItemControl": "row",
        "SliderControl": "slider"}


class Element:
    def __init__(self, name, kind, rect, control):
        self.name, self.kind, self.rect, self.control = name, kind, rect, control

    @property
    def center(self):
        return ((self.rect[0] + self.rect[2]) // 2, (self.rect[1] + self.rect[3]) // 2)

    def line(self):
        return f"{self.kind} \"{self.name}\""


def foreground():
    hwnd = user32.GetForegroundWindow()
    return auto.ControlFromHandle(hwnd) if hwnd else None


def page(root):
    """The web page (or document) inside a browser / editor window, if it exposes one: its content, without the tab strip."""
    try:
        doc = auto.DocumentControl(searchFromControl=root, searchDepth=12)
        if doc.Exists(0.4, 0):
            return doc
    except Exception:                                                              # noqa: BLE001
        pass
    return None


def elements(root=None, limit=70, budget=1.2, scope="auto"):
    """Interactive elements of the foreground window, in reading order, within a time budget (big apps have huge trees).
    scope: 'page' = only the web page / document, 'window' = the whole window, 'auto' = the page if there is one."""
    root = root or foreground()
    if root is None:
        return []
    if scope in ("auto", "page"):
        doc = page(root)
        if doc is not None:
            root = doc
        elif scope == "page":
            return []
    found, started = [], time.monotonic()
    try:
        for control, depth in auto.WalkControl(root, includeTop=False, maxDepth=14):
            if time.monotonic() - started > budget or len(found) >= limit * 3:
                break
            try:
                if control.ControlTypeName not in INTERACTIVE or control.IsOffscreen:
                    continue
                name = (control.Name or "").strip()
                if not name and control.ControlTypeName != "EditControl":
                    continue
                r = control.BoundingRectangle
                if r.width() < 4 or r.height() < 4:
                    continue
                found.append(Element(name[:70], KIND.get(control.ControlTypeName, "item"), (r.left, r.top, r.right, r.bottom), control))
            except Exception:                                                     # noqa: BLE001  (the element vanished while walking)
                continue
    except Exception:                                                              # noqa: BLE001
        pass
    found.sort(key=lambda e: (e.rect[1] // 30, e.rect[0]))                         # top to bottom, left to right
    seen, unique = set(), []
    for e in found:                                                                # the same name and place twice is noise
        key = (e.name, e.rect[0] // 8, e.rect[1] // 8)
        if key not in seen:
            seen.add(key)
            unique.append(e)
    return unique[:limit]


def describe(items):
    """The element list as compact text for the model: numbered so it can say 'click 7'."""
    return "\n".join(f"{i + 1}. {e.line()}" for i, e in enumerate(items))


def find(name, items=None, kind=None):
    """The element whose name matches best (exact, then contains, then fuzzy)."""
    import difflib
    items = items if items is not None else elements()
    want = name.lower().strip()
    pool = [e for e in items if not kind or e.kind == kind]
    for test in (lambda n: n == want, lambda n: n.startswith(want), lambda n: want in n):
        hit = [e for e in pool if test(e.name.lower())]
        if hit:
            return min(hit, key=lambda e: len(e.name))
    names = [e.name.lower() for e in pool]
    close = difflib.get_close_matches(want, names, n=1, cutoff=0.72)
    return next((e for e in pool if e.name.lower() == close[0]), None) if close else None

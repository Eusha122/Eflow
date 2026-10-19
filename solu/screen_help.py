"""Screen help: questions about what is on the screen, answered from OCR text or, for pictures, the vision model."""
import re

from prompts import CODE_NOTE, SCREEN_NOTE, SCREEN_ONLY_LOOK, TRANSLATE_NOTE
import screen as screen_mod


class ScreenHelpMixin:
    """Looking at the screen. Mixed into Solu."""

    # ---- looking ---------------------------------------------------------------------------------------------
    SCREEN_NOUNS = r"screen|page|error|warning|code|text|message|email|window|document|image|picture|photo|article|website|tab|dialog|popup|pop up|button|menu|chart|graph|table|spreadsheet|slide|video|file|terminal|console|log|stack trace|exception|bug|result|form"

    SCREEN_ASKS = r"what|why|how|explain|read|translate|summari[sz]e|describe|help|fix|mean|say|tell|check|look|see|wrong|going on|solve|answer|reply|respond"

    def is_screen_request(self, text):
        """True for "look at this", "what does this error mean", "what's on my screen" ... (not for "look up the weather")."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        if not t:
            return False
        if re.match(r"^(look|have a look|take a look)\b(?!\s+(up|for|after|out|forward)\b)", t):
            return True
        if re.search(r"\bwhy (is|does|did) (this|it|my (code|build|program|app))( keep)? (fail|failing|crash|crashing|break|breaking|not work)|"
                     r"\bfix (this|the) (error|bug|problem)\b|\bwhat('?s| is) wrong with (this|my) (code|build|program)\b|"
                     r"\btranslate (this|that|it|the (page|text|screen|message|email))\b", t):
            return True
        if re.search(r"\b(what am i looking at|can you see (this|my screen|it|that)|what('?s| is) on my screen|what('?s| is) happening on my screen|help me with this|what('?s| is) wrong (with )?(this|here)|what does (this|that|it) (say|mean))\b", t):
            return True
        return bool(re.search(r"\b(this|that|these|those|here|my screen|the screen|on screen)\b", t)
                    and re.search(rf"\b({self.SCREEN_NOUNS})\b", t) and re.search(rf"\b({self.SCREEN_ASKS})", t))

    @staticmethod
    def needs_vision(question, ocr_text):
        """Text questions are answered from what Windows read (fast); pictures need the vision model (slower: it swaps in)."""
        if len(ocr_text) < 30:
            return True
        return bool(re.search(r"\b(picture|photo|image|logo|colou?r|looks? like|drawing|chart|graph|diagram|meme|video|who is|who's|what animal|what is in|what's in)\b", question.lower()))

    def respond_screen(self, text):
        """Look at the window the user is in, then answer their question about it."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        if not self.screen_enabled:
            return self.respond_text("Screen help is switched off. You can turn it on in Settings, or tell me to turn on screen help.")
        whole = bool(re.search(r"\b(whole|entire|all|full|both)\b.*\bscreens?\b|\ball my (screens|monitors)\b", t))
        look = screen_mod.look_at_foreground(whole)
        if look.blocked:
            return self.respond_text(look.blocked)
        self._answer_look(look, text)

    def _answer_look(self, look, text, mode=None):
        """Answer `text` about a captured window or area: explain, translate, help with an error."""
        t = " ".join(re.findall(r"[a-z0-9']+", text.lower()))
        self.event("looking", title=look.title, app=look.app, scope=look.scope, thumb=screen_mod.thumbnail_b64(look))
        ocr = screen_mod.read_text(look)
        question, note = text, SCREEN_NOTE
        if re.fullmatch(r"(look( at (this|that|it|my screen|the screen))?|take a look|have a look|see this|what('?s| is) on my screen|can you see (this|it|that|my screen))", t):
            question = SCREEN_ONLY_LOOK
        if mode == "translate" or re.search(r"\btranslat", t):
            to = re.search(r"\b(?:to|into)\s+(bangla|bengali|english|hindi|urdu|arabic|spanish|french|german|chinese|japanese|korean)\b", t)
            target = f"into {to.group(1).capitalize()}" if to else "into English (or into Bangla if the text is already English)"
            question, note = f"Translate the text on this screen {target}.", TRANSLATE_NOTE.format(target=target)
        elif mode == "explain" and t in ("", "explain this", "look at this"):
            question = "Explain what this part of the screen shows and anything important in it."
        elif re.search(r"\b(error|bug|fail|failing|crash|exception|not working|broken|fix)\b", t):
            note = CODE_NOTE
        translating = question.startswith("Translate the text")
        vlm = self.needs_vision(question, ocr) and not translating
        screen = {"title": look.title, "app": look.app, "scope": look.scope, "text": ocr, "vlm": vlm, "note": note,
                  "image": screen_mod.picture_b64(look) if vlm else None}
        print(f"   looking at: {look.app} \u2014 {look.title!r} ({len(ocr)} characters read, {'picture' if vlm else 'text'} mode)")
        if vlm:
            self.cancel.clear()
            self._speaking_reported = False
            self.speaking.set()
            self.say("One moment, I'll take a closer look.")                 # the vision model takes a few seconds to wake up
        try:
            self._respond(question, screen)
        except Exception as e:                                                # noqa: BLE001
            print(f"   (looking failed: {e})")
            self.event("error", message="Solu couldn't look at the screen. Is Ollama running?")
        finally:
            self._after_speaking()

    def respond_region(self, x, y, w, h, mode="explain"):
        from PIL import ImageGrab
        try:
            self.cancel.clear()
            image = ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True).convert("RGB")
            look = screen_mod.Look("the area you selected", "screen", image, "region")
            look.origin = (x, y)
            question = "Translate this." if mode == "translate" else "Explain this."
            self.start_conversation()
            self.event("heard", text="(the area you showed me)")
            self.event("state", state="thinking")
            self._answer_look(look, question, mode)
        except Exception as e:                                                # noqa: BLE001
            print(f"   (region failed: {e})")
            self.respond_text("Sorry, I couldn't read that part of the screen.")

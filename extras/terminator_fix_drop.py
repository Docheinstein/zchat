"""Fixes dropping files on Terminator 2.1.3.

Its drag and drop handler passes bytes to Terminal.feed(), which encodes them again and fails with
"'bytes' object has no attribute 'encode'", so nothing is typed. This makes feed() accept bytes too.
Terminator imports every plugin file, enabled or not, so nothing needs to be enabled.
Safe to delete once Terminator is updated to a version with the fix.
"""

from terminatorlib import terminal

_feed = terminal.Terminal.feed


def _feed_text_or_bytes(self, text):
    if isinstance(text, bytes):
        text = text.decode('utf-8', 'replace')
    _feed(self, text)


if not getattr(_feed, '_accepts_bytes', False):
    _feed_text_or_bytes._accepts_bytes = True
    terminal.Terminal.feed = _feed_text_or_bytes

AVAILABLE = []

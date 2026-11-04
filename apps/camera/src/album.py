"""The camera test's album: the photos and screenshots in the cart's saves, as thumbnails, one at a time full
screen, deleted. Images load in the background (e.load_texture_async), at most two at a time: a full-size photo
decodes to about 32 MB before it's scaled down, and takes about a second on the handheld."""

import alia_engine as ae

COLS, ROWS = 4, 3
THUMB_W, THUMB_H, GAP = 140, 96, 10
WHITE, DIM, SHADOW, PICK = 0xFFFFFFFF, 0xA0A8B0FF, 0x000000C0, 0xFFC040FF
LOADING_AT_ONCE = 2


def pictures():
    """photo-N.jpg and screen-N.png in the saves, newest first."""
    def key(name):
        stem = name.rsplit(".", 1)[0]
        kind, _, num = stem.partition("-")
        return (int(num) if num.isdigit() else 0, kind)
    names = [n for n in ae.save.names() if n.endswith((".jpg", ".png")) and n.startswith(("photo-", "screen-"))]
    return sorted(names, key=key, reverse=True)


class Album:
    def __init__(self, e):
        self.e = e
        self.names = pictures()
        self.pick = 0
        self.viewing = None      # the name shown full screen
        self.full = None         # its texture
        self.thumbs = {}         # name -> Texture (this page's)
        self.queue = []          # names waiting to load
        self.confirm = False     # Y pressed once: press again to delete
        self.stick = (0, 0)      # the stick's last direction, so one push moves one step
        self.note = ""

    # -- input -------------------------------------------------------------------------------------------
    def step(self, i):
        """One logic step; False when the album closes."""
        move = self._direction(i)
        if self.viewing:
            if i.pressed & (ae.EAST | ae.SOUTH):
                self._close_full()
            elif move[0]:
                self.pick = (self.pick + move[0]) % max(1, len(self.names))
                self._open_full()
            return True
        if i.pressed & ae.SOUTH:
            self.close()
            return False
        if not self.names:
            return True
        if move != (0, 0):
            self.confirm = False
            self.pick = max(0, min(len(self.names) - 1, self.pick + move[0] + move[1] * COLS))
        if i.pressed & ae.EAST:
            self._open_full()
        if i.pressed & ae.WEST:
            if self.confirm:
                self._delete(self.names[self.pick])
            else:
                self.confirm, self.note = True, f"Y again: delete {self.names[self.pick]}"
        return True

    def _direction(self, i):
        d = (0, 0)
        if i.pressed & ae.LEFT: d = (-1, 0)
        if i.pressed & ae.RIGHT: d = (1, 0)
        if i.pressed & ae.UP: d = (0, -1)
        if i.pressed & ae.DOWN: d = (0, 1)
        s = (-1 if i.lx < -0.6 else 1 if i.lx > 0.6 else 0, -1 if i.ly < -0.6 else 1 if i.ly > 0.6 else 0)
        if s != self.stick and s != (0, 0) and d == (0, 0):
            d = (s[0], 0) if s[0] else (0, s[1])
        self.stick = s
        return d

    # -- loading -------------------------------------------------------------------------------------------
    def _load(self, name, max_size):
        data = ae.save.read(name)
        if data is None:
            return None
        return self.e.load_texture_async(data, max_size=max_size, filter="linear")

    def update(self):
        """Each frame: start loads for this page's thumbnails, two at a time; free other pages'."""
        page = self.pick // (COLS * ROWS)
        shown = self.names[page * COLS * ROWS:(page + 1) * COLS * ROWS]
        for name in list(self.thumbs):
            if name not in shown:
                self.thumbs.pop(name).destroy()
        loading = sum(1 for t in self.thumbs.values() if not t.done) + (1 if self.full and not self.full.done else 0)
        for name in shown:
            if loading >= LOADING_AT_ONCE:
                break
            if name not in self.thumbs:
                t = self._load(name, THUMB_W * 2)
                if t:
                    self.thumbs[name] = t
                    loading += 1

    def _open_full(self):
        if not self.names:
            return
        self._close_full()
        self.viewing = self.names[self.pick]
        self.full = self._load(self.viewing, 1024)

    def _close_full(self):
        if self.full:
            self.full.destroy()
        self.viewing, self.full = None, None

    def _delete(self, name):
        try:
            ae.save.delete(name)
            self.note = f"deleted {name}"
        except OSError as err:
            self.note = f"delete failed: {err}"
        if name in self.thumbs:
            self.thumbs.pop(name).destroy()
        self.names = pictures()
        self.pick = min(self.pick, max(0, len(self.names) - 1))
        self.confirm = False

    def close(self):
        self._close_full()
        for t in self.thumbs.values():
            t.destroy()
        self.thumbs.clear()

    # -- drawing -------------------------------------------------------------------------------------------
    def render(self):
        e = self.e
        w, h = e.scene_size
        e.hud_rect(0, 0, w, h, 0x101418FF)
        if self.viewing:
            if self.full and self.full.ready:
                e.hud_image(self.full, 0, 0, w, h - 14, fit=True)
            else:
                e.hud_text(w / 2 - 30, h / 2, "LOADING..." if not (self.full and self.full.error) else "CAN'T READ IT", WHITE, 1)
            e.hud_rect(0, h - 14, w, 14, 0x000000C0)
            e.hud_text(4, h - 11, f"{self.viewing}  ({self.pick + 1}/{len(self.names)})   LEFT/RIGHT: NEXT   A/B: BACK",
                       DIM, 1, SHADOW)
            return
        e.hud_text(8, 6, f"ALBUM  {len(self.names)} PICTURES", WHITE, 1, SHADOW)
        if not self.names:
            e.hud_text(8, 30, "nothing yet: A takes a photo, SELECT a screenshot", DIM, 1)
        page = self.pick // (COLS * ROWS)
        x0 = (w - (COLS * THUMB_W + (COLS - 1) * GAP)) / 2
        for k, name in enumerate(self.names[page * COLS * ROWS:(page + 1) * COLS * ROWS]):
            x, y = x0 + (k % COLS) * (THUMB_W + GAP), 22 + (k // COLS) * (THUMB_H + GAP + 8)
            index = page * COLS * ROWS + k
            if index == self.pick:
                e.hud_rect(x - 2, y - 2, THUMB_W + 4, THUMB_H + 4, PICK)
            e.hud_rect(x, y, THUMB_W, THUMB_H, 0x000000FF)
            t = self.thumbs.get(name)
            if t and t.ready:
                e.hud_image(t, x, y, THUMB_W, THUMB_H)
            else:
                e.hud_text(x + 4, y + THUMB_H / 2 - 4, "..." if not (t and t.error) else "?", DIM, 1)
            e.hud_text(x, y + THUMB_H + 2, name, WHITE if index == self.pick else DIM, 1)
        pages = (len(self.names) + COLS * ROWS - 1) // (COLS * ROWS)
        e.hud_rect(0, h - 14, w, 14, 0x000000C0)
        e.hud_text(4, h - 11, self.note or f"PAGE {page + 1}/{max(pages, 1)}   STICK/D-PAD: PICK   A: VIEW   Y: DELETE   B: BACK",
                   DIM, 1, SHADOW)

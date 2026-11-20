#!/usr/bin/env python3
"""Build romfs/gfx/ from the art extracted from Tomodachi Life.

    python tools/assets/build_gfx.py [source folder]

The source folder (default: assetsfromthegametouseinthetomotoolnxapp/ at the
repository root) holds PNGs and zips of PNGs taken from the game. This picks
only what the app uses, and writes small files into romfs/gfx/ (built into the
.nro, and served to the WebUI at /gfx/):

  logo.png            the TomoToolNX mark (cropped)
  logo_tomodachishare.png  TomodachiShare's picture (its button)
  pat_*.png           tiling patterns as white-on-clear masks (wallpapers)
  art_*.png, kind_*   glossy illustrations (big moments, texture kinds)
  ico_*.png           line icons as white masks (tinted by the theme)
  face_*.png          Mii avatar faces, centred on the head; face_*_hair.png is
                      where the hair is (tinted with each Mii's hair colour)
  hand_*_N.png        hands in the six skin tones of the Player page
  shop_*.png          shop logos (texture kinds), news_*.{png,jpg}

The app draws its own icons wherever one of these is missing, so the build
works without them. Needs Pillow.
"""
import io
import os
import sys
import zipfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assetsfromthegametouseinthetomotoolnxapp")
OUT = os.path.join(ROOT, "romfs", "gfx")

_zips = {}


def load(path):
    """'Folder.zip/name.png' or 'name.png' inside the source folder."""
    if ".zip/" in path:
        zname, inner = path.split(".zip/", 1)
        z = _zips.get(zname)
        if z is None:
            z = _zips[zname] = zipfile.ZipFile(os.path.join(SRC, zname + ".zip"))
        names = {os.path.basename(n): n for n in z.namelist()}
        return Image.open(io.BytesIO(z.read(names[inner]))).convert("RGBA")
    return Image.open(os.path.join(SRC, path)).convert("RGBA")


def crop(im):
    box = im.getchannel("A").getbbox()
    return im.crop(box) if box else im


def fit(im, w, h):
    im = im.copy()
    im.thumbnail((w, h), Image.LANCZOS)
    return im


HEAD = 0.38   # a face's round head, as a share of its square image's width


def chamfer(inside, w, h):
    """3-4 chamfer distance (in thirds of a pixel) from each inside pixel to
    the nearest outside one; 0 outside."""
    big = w * h * 3
    d = [[big if inside[y][x] else 0 for x in range(w)] for y in range(h)]
    for passes in (((-1, 0, 3), (0, -1, 3), (-1, -1, 4), (1, -1, 4)),
                   ((1, 0, 3), (0, 1, 3), (1, 1, 4), (-1, 1, 4))):
        ys = range(h) if passes[0][0] < 0 else range(h - 1, -1, -1)
        xs = range(w) if passes[0][0] < 0 else range(w - 1, -1, -1)
        for y in ys:
            row = d[y]
            for x in xs:
                if row[x]:
                    best = row[x]
                    for dx, dy, c in passes:
                        nx, ny = x + dx, y + dy
                        v = d[ny][nx] + c if 0 <= nx < w and 0 <= ny < h else c
                        if v < best:
                            best = v
                    row[x] = best
    return d


def head_square(im):
    """A Mii editor face re-centred on its round head. The faces look three
    quarters to the left, with an ear, a bow, a tuft or long hair sticking out,
    so centring the whole picture leaves the head off-centre in a circle. The
    head is the largest circle that fits inside the shape (a distance
    transform's peak); the square is centred on it, with the head's radius at
    HEAD of the width, so the app can draw every face the same way."""
    im = crop(im)
    w, h = im.size
    a = im.getchannel("A").load()
    d = chamfer([[a[x, y] > 128 for x in range(w)] for y in range(h)], w, h)
    r, cx, cy = max((d[y][x], x, y) for y in range(h) for x in range(w))
    half = r / 3.0 / HEAD / 2
    box = (round(cx - half), round(cy - half), round(cx + half), round(cy + half))
    out = Image.new("RGBA", (box[2] - box[0], box[3] - box[1]), (0, 0, 0, 0))
    out.paste(im, (-box[0], -box[1]), im)
    return out


def hair_mask(im):
    """Where a face's hair is, as a white mask, to be tinted with the Mii's own
    hair colour. The faces are black lines on white: the hair is the dark area
    that reaches the outside (the eyes and mouth are enclosed by skin). The
    head's outline reaches the outside too, but it is a band of even width
    (about w / 15), while hair is much thicker: only what is deeper than that
    band is kept, grown back to the hair's shape (an opening), minus a thin rim
    that stays as the outline."""
    w, h = im.size
    px = im.load()
    dark = [[px[x, y][3] > 128 and sum(px[x, y][:3]) < 3 * 90 for x in range(w)] for y in range(h)]
    clear = [[px[x, y][3] <= 128 for x in range(w)] for y in range(h)]
    # Dark areas touching the outside: flood from every dark pixel next to a
    # clear one (or the edge).
    hair = [[False] * w for _ in range(h)]
    stack = []
    for y in range(h):
        for x in range(w):
            if dark[y][x] and any(not (0 <= x + dx < w and 0 <= y + dy < h) or clear[y + dy][x + dx]
                                  for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                hair[y][x] = True
                stack.append((x, y))
    while stack:
        x, y = stack.pop()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and dark[ny][nx] and not hair[ny][nx]:
                hair[ny][nx] = True
                stack.append((nx, ny))
    # The hair's core: deeper than half the outline band, with room to spare.
    d = chamfer(hair, w, h)
    rim = 3 * max(2, round(w / 40))
    deep = 3 * max(3, round(w / 22))
    core = [[d[y][x] > deep for x in range(w)] for y in range(h)]
    # Grown back to the hair's edge less the rim (the outline stays dark, and
    # the outline band only takes the colour for a pixel or two where it meets
    # the hair).
    near = chamfer([[not core[y][x] for x in range(w)] for y in range(h)], w, h)
    m = Image.new("L", (w, h), 0)
    mp = m.load()
    for y in range(h):
        for x in range(w):
            if hair[y][x] and d[y][x] > rim and (core[y][x] or near[y][x] <= deep - rim):
                mp[x, y] = 255
    white = Image.new("L", (w, h), 255)
    return Image.merge("LA", (white, m))


def mask(im):
    """White shapes on clear: alpha is the old alpha times the brightness, so
    white-on-black patterns and white-on-clear icons both come out right."""
    r, g, b, a = im.split()
    lum = Image.merge("RGB", (r, g, b)).convert("L")
    alpha = Image.composite(lum, Image.new("L", im.size, 0), a)    # lum × a / 255
    white = Image.new("L", im.size, 255)
    return Image.merge("LA", (white, alpha))


def save(im, name, quality=None):
    path = os.path.join(OUT, name)
    if name.endswith(".jpg"):
        im.convert("RGB").save(path, "JPEG", quality=quality or 84, optimize=True, progressive=True)
    else:
        im.save(path, "PNG", optimize=True)
    return os.path.getsize(path)


PATTERNS = {
    "pat_face":       ("CmnFacePartsBG_00u.png", 384),
    "pat_itembook":   ("ItemBookBGPattern_00r.png", 385),
    "pat_building":   ("MiiPresentBG_Buildingr.png", 384),
    "pat_clothes":    ("MiiPresentBG_Clothesr.png", 384),
    "pat_coordinate": ("MiiPresentBG_Coordinater.png", 384),
    "pat_food":       ("MiiPresentBG_Foodr.png", 384),
    "pat_goods":      ("MiiPresentBG_Goodsr.png", 384),
    "pat_ground":     ("MiiPresentBG_Groundr.png", 384),
    "pat_house":      ("MiiPresentBG_Houser.png", 384),
    "pat_islandedit": ("MiiPresentBG_IslandEditr.png", 384),
    "pat_photo":      ("MiiPresentBG_Photor.png", 384),
    "pat_quiz":       ("PictQuizOPBGs.png", 384),
}
ART = {
    "art_atelier":   "UnLockIcon_Atelierw.png",
    "art_quikbuild": "UnLockIcon_BuildingShopw.png",
    "art_floor":     "UnLockIcon_Floorw.png",
    "art_reno":      "UnLockIcon_InteriorShopw.png",
    "art_goods":     "UnLockIcon_LevelUpGoodsw.png",
    "art_mii":       "UnLockIcon_Petitw.png",
    "art_travel":    "UnLockIcon_Travelw.png",
    "kind_clothes":  "UGCEditor.zip/UGCUnlockAtelierIcon_Clothes^w.png",
    "kind_exterior": "UGCEditor.zip/UGCUnlockAtelierIcon_Exterior^w.png",
    "kind_floor":    "UGCEditor.zip/UGCUnlockAtelierIcon_Floor^w.png",
    "kind_goods":    "UGCEditor.zip/UGCUnlockAtelierIcon_Goods^w.png",
    "kind_interior": "UGCEditor.zip/UGCUnlockAtelierIcon_Interior^w.png",
    "kind_obj":      "UGCEditor.zip/UGCUnlockAtelierIcon_OBJ^w.png",
    "kind_food":     "UGCEditor.zip/UGCEditor_IconCategoryAtelier_00^w.png",
}
ICONS = {
    "ico_home":        "UGCEditor.zip/UGCEditor_IconSetting_10^s.png",
    "ico_miis":        "EditorCommon.zip/EditorCommon_Icon_Title_Family^s.png",
    "ico_textures":    "UGCEditor.zip/UGCEditor_IconStampBase^s.png",
    "ico_player":      "EditorCommon.zip/EditorCommon_Icon_Title_BodyType^s.png",
    "ico_map":         "UGCEditor.zip/UGCEditor_IconMapObjSet00^s.png",
    "ico_backups":     "UGCEditor.zip/UGCEditor_IconStampHistory^s.png",
    "ico_phone":       "UGCEditor.zip/UGCEditor_IconSetting_04^w.png",
    "ico_settings":    "UGCEditor.zip/UGCEditor_IconSetting_00^s.png",
    "ico_face":        "EditorCommon.zip/EditorCommon_Icon_Title_Gender^s.png",
    "ico_words":       "EditorCommon.zip/EditorCommon_Icon_Title_Phrase00^s.png",
    "ico_items":       "EditorCommon.zip/EditorCommon_Icon_Title_EventStyle^s.png",
    "ico_habits":      "EditorCommon.zip/EditorCommon_Icon_Title_MiiPartsList^s.png",
    "ico_social":      "EditorCommon.zip/EditorCommon_Icon_Title_LoveTarget^s.png",
    "ico_birthday":    "EditorCommon.zip/EditorCommon_Icon_Title_Birthday^s.png",
    "ico_name":        "EditorCommon.zip/EditorCommon_Icon_Title_Name^s.png",
    "ico_personality": "EditorCommon.zip/EditorCommon_Icon_Title_Personality^s.png",
    "ico_voice":       "EditorCommon.zip/EditorCommon_Icon_Title_Voice^s.png",
    "ico_look":        "UGCEditor.zip/UGCEditor_IconSetting_05^s.png",
    "ico_language":    "MiniGames.zip/MiniGameTitleIcon_Balloon^s.png",
    "ico_pictures":    "UGCEditor.zip/UGCEditor_IconStampCapture^s.png",
    "ico_about":       "MiniGames.zip/MiniGameTitleIcon_Hatena^s.png",
    "ico_trash":       "UGCEditor.zip/UGCEditor_IconMove10^w.png",
    "ico_search":      "UGCEditor.zip/UGCEditor_IconScaling^w.png",
    "ico_undo":        "UGCEditor.zip/UGCEditor_IconUndo^d.png",
    "ico_sparkle":     "UGCEditor.zip/UGCEditor_Icon08^d.png",
    "ico_pen":         "UGCEditor.zip/UGCEditor_Icon01^d.png",
    "ico_grid":        "UGCEditor.zip/UGCEditor_IconGrid01^w.png",
    "ico_fullscreen":  "UGCEditor.zip/UGCEditor_IconFullScreen^w.png",
    "ico_select":      "UGCEditor.zip/UGCEditor_Icon05^d.png",
    "ico_bucket":      "UGCEditor.zip/UGCEditor_Icon02^d.png",
    "ico_zoom":        "MiniGames.zip/MiniGameTitleIcon_Zoom^s.png",
    "ico_rotate":      "UGCEditor.zip/UGCEditor_IconMove09^w.png",
    "ico_move":        "UGCEditor.zip/UGCEditor_IconShortCut2^w.png",
    "ico_island":      "MiiEditor.zip/MiiEditor_Icon_IslandMii_00^s.png",
    "ico_camera":      "UGCEditor.zip/UGCEditor_IconSetting_03^s.png",
}
FACES = {
    "face_boy":   "MiiEditor.zip/MiiEditor_Icon_VoicePattern_Alpha00^t.png",
    "face_girl":  "MiiEditor.zip/MiiEditor_Icon_VoicePattern_Alpha01^t.png",
    "face_man":   "MiiEditor.zip/MiiEditor_Icon_VoicePattern_Alpha02^t.png",
    "face_woman": "MiiEditor.zip/MiiEditor_Icon_VoicePattern_Alpha03^t.png",
    "face_old":   "MiiEditor.zip/MiiEditor_Icon_VoicePattern_Alpha04^t.png",
    "face_kid":   "MiiEditor.zip/MiiEditor_Icon_VoicePattern_Alpha05^t.png",
}
SHOPS = {
    "shop_food":      ("Logos.zip/LogoFoodShop_L^w.png", 220, 140),
    "shop_clothes":   ("Logos.zip/LogoClothShop_00^s.png", 260, 64),
    "shop_goods":     ("Logos.zip/LogoMyGoodsShop_00^w.png", 240, 100),
    "shop_reno":      ("Logos.zip/LogoInteriorShop_00^s.png", 260, 64),
    "shop_quikbuild": ("Logos.zip/LogoBuildingShop_L^w.png", 150, 160),
    "shop_atelier":   ("Logos.zip/LogoAtelier_00^w.png", 300, 72),
    "news_logo":      ("Logos.zip/NewsLogo_LogoS^s.png", 128, 128),
}


def main():
    if not os.path.isdir(SRC):
        sys.exit("source folder not found: " + SRC)
    os.makedirs(OUT, exist_ok=True)
    total, n = 0, 0

    total += save(fit(crop(load("tomotoolnxlogonobackground.png")), 256, 256), "logo.png"); n += 1
    # TomodachiShare's own picture, for its button.
    total += save(fit(crop(load("tomodachishare.png")), 132, 132), "logo_tomodachishare.png"); n += 1
    for name, (src, w) in PATTERNS.items():
        im = load(src)
        im = im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)
        total += save(mask(im), name + ".png"); n += 1
    for name, src in ART.items():
        total += save(fit(crop(load(src)), 192, 192), name + ".png"); n += 1
    for name, src in ICONS.items():
        total += save(mask(fit(crop(load(src)), 64, 64)), name + ".png"); n += 1
    for name, src in FACES.items():
        face = head_square(load(src))
        total += save(face.resize((112, 112), Image.LANCZOS), name + ".png")
        total += save(hair_mask(face).resize((112, 112), Image.LANCZOS), name + "_hair.png")
        n += 2
    for i in range(6):
        total += save(fit(crop(load("all hands.zip/Hand_PushPointing_%02d^w.png" % i)), 420, 140), "hand_point_%d.png" % i)
        total += save(fit(crop(load("all hands.zip/Hand_Pick_Open%02d^w.png" % i)), 120, 260), "hand_open_%d.png" % i)
        total += save(fit(crop(load("all hands.zip/Hand_Pick_Close%02d^w.png" % i)), 120, 260), "hand_close_%d.png" % i)
        n += 3
    for name, (src, w, h) in SHOPS.items():
        total += save(fit(crop(load(src)), w, h), name + ".png"); n += 1
    bg = load("NewsBGBack_00o.png")
    total += save(bg.resize((960, round(bg.height * 960 / bg.width)), Image.LANCZOS), "news_bg.jpg", 82); n += 1

    print("%d files, %.0f KB in %s" % (n, total / 1024, os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()

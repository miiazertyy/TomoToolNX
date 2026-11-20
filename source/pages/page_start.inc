// page_start.inc — TomoToolNX 2.0: everything before an island is open.
// Choose a user (with each user's backups), the backup question, the backup
// in progress, errors, updates, the applet warning, "Saved!", and making a new
// island. Included by shell.inc inside namespace S2.

// Draw one frame right now: for work that blocks (mounting, restoring).
static void BusyFrame(const std::string& title, const std::string& sub = std::string()) {
    ClearScreen();
    W::Spinner(SCREEN_W / 2, SCREEN_H / 2 - 44, 28, gTheme.gold);
    DrawTextC(title, SCREEN_W / 2, SCREEN_H / 2 + 22, gTheme.text, Font::Title);
    if (!sub.empty()) DrawTextC(sub, SCREEN_W / 2, SCREEN_H / 2 + 58, gTheme.dim, Font::Body);
    SDL_RenderPresent(gRen);
}

// A centred card for one-message pages (errors, updates). Returns its rect.
static SDL_Rect MessageCard(int w, int h) {
    const SDL_Rect r{ (SCREEN_W - w) / 2, (SCREEN_H - h) / 2 - 16, w, h };
    W::BeginBlock(r);
    FillRectRounded(r.x, r.y + 12, r.w, r.h, 28, SDL_Color{ 0, 0, 0, 55 });
    FillRectRounded(r.x, r.y, r.w, r.h, 28, Alpha(gTheme.panel, 236));
    DrawRectRounded(r.x, r.y, r.w, r.h, 28, Alpha(gTheme.border, 220));
    return r;
}

static void TopBrand() {
    Brand(40, 34, "v" APP_VERSION);
}

// ── Backups of a user ────────────────────────────────────────────────────────
static std::vector<int> sBkCounts;
static double sBkCountsAt = -10;
static void RefreshBackupCounts(bool force) {
    const int n = (int)gUsers.size();
    if (!force && (int)sBkCounts.size() == n && Motion::Now() - sBkCountsAt < 3.0) return;
    sBkCounts.assign(n, 0);
    for (int i = 0; i < n; i++) sBkCounts[i] = BackupService::CountBackups(SaveMount::SafeUserFolder(gUsers[i]));
    sBkCountsAt = Motion::Now();
}
static std::string BackupsLabel(int n) {
    return n == 0 ? Tx("userpick.nobackups") : n == 1 ? Tx("userpick.backup1")
                  : TokenFill(Tx("userpick.backups"), "{n}", std::to_string(n));
}

static int sBkMenu = -1;       // the backup whose menu is open
static int sBkAsk = 0;         // 1 restore, 2 delete: the dialog asking first
static int sBkAskIdx = -1;

static void OpenBackups(int u) {
    if (u < 0 || u >= (int)gUsers.size()) return;
    gRestorePickerUserIdx = u;
    gRestorePickerUid = SaveMount::SafeUserFolder(gUsers[u]);
    gRestoreList = BackupService::ListBackups(gRestorePickerUid);
    gRestoreSel = 0; gRestoreScroll = 0; gRestoreConfirmKind = 0;
    sBkMenu = -1; sBkAsk = 0;
    gShowRestorePicker = true;
    Motion::Forget(Ui::HashStr("bk.sheet"));
}
static void CloseBackups() {
    gShowRestorePicker = false;
    sBkMenu = -1; sBkAsk = 0;
    RefreshBackupCounts(true);
    if (gRestoreFromHome) ReturnToEditorAfterRestore(gRestorePickerUserIdx, "", Ui::Tone::Neutral);
}

static std::string BackupWhen(const std::string& path, std::string* time = nullptr) {
    const BackupTs t = ParseBackupName(path);
    if (!t.ok) {
        const size_t sl = path.rfind('/');
        return sl != std::string::npos ? path.substr(sl + 1) : path;
    }
    if (time) { char h[16]; snprintf(h, sizeof h, "%02d:%02d", t.hour, t.min); *time = h; }
    return DateText(t.year, t.mon, t.day);
}

// ── A backup in full ─────────────────────────────────────────────────────────
// A number with the language's decimal mark ("4.4", "4,4").
static std::string DecimalText(double v, int decimals) {
    char s[32];
    snprintf(s, sizeof s, "%.*f", decimals, v);
    std::string out = s;
    const std::string mark = Tx("num.dec");
    const size_t p = out.find('.');
    if (p != std::string::npos && mark != ".") out.replace(p, 1, mark);
    return out;
}
// A size on the SD card ("4.4 MB").
static std::string SizeText(uint64_t b) {
    const double mb = 1024.0 * 1024.0, gb = mb * 1024.0;
    if (b < (uint64_t)mb) return std::to_string((unsigned long long)((b + 1023) / 1024)) + " " + Tx("unit.kb");
    if (b < (uint64_t)gb) { const double v = b / mb; return DecimalText(v, v < 100 ? 1 : 0) + " " + Tx("unit.mb"); }
    return DecimalText(b / gb, 2) + " " + Tx("unit.gb");
}
static std::string CountText(int n, const char* one, const char* many) {
    return n == 1 ? Tx(one) : TokenFill(Tx(many), "{n}", std::to_string(n));
}
// How long ago a backup was made ("2 h ago", "Yesterday"). Empty when it is
// over a month old (its date says it better) or its name holds no date.
static std::string BackupAge(const BackupTs& t) {
    if (!t.ok) return std::string();
    struct tm b {};
    b.tm_year = t.year - 1900; b.tm_mon = t.mon - 1; b.tm_mday = t.day;
    b.tm_hour = t.hour; b.tm_min = t.min; b.tm_isdst = -1;
    struct tm b0 = b;
    const time_t then = mktime(&b), now = time(nullptr);
    if (then == (time_t)-1) return std::string();
    const double s = difftime(now, then);
    if (s < -120) return std::string();      // the clock went back since
    if (s < 60) return Tx("bk.ago.now");
    if (s < 3600) return TokenFill(Tx("bk.ago.min"), "{n}", std::to_string((int)(s / 60)));
    struct tm n0 = *localtime(&now);
    n0.tm_hour = n0.tm_min = n0.tm_sec = 0; n0.tm_isdst = -1;
    b0.tm_hour = b0.tm_min = b0.tm_sec = 0;
    const int days = (int)lround(difftime(mktime(&n0), mktime(&b0)) / 86400.0);
    if (days <= 0) return TokenFill(Tx("bk.ago.hour"), "{n}", std::to_string((int)(s / 3600)));
    if (days == 1) return Tx("bk.ago.yesterday");
    if (days <= 30) return TokenFill(Tx("bk.ago.days"), "{n}", std::to_string(days));
    return std::string();
}
// One of the game's icons and a few words, on one line. Returns the width.
static int InfoBit(int x, int cy, Art::Id icon, const std::string& text, SDL_Color c) {
    int w = 0;
    if (Art::Icon(icon, x + 9, cy, 20, c)) w = 26;
    DrawTextVC(text, x + w, cy, c, Font::Sm);
    return w + TextW(text, Font::Sm);
}

// One backup as a card: a calendar leaf with its day, how long ago and when,
// what is inside (read in the background), its size, and Newest / Oldest.
// The node is registered even when `visible` is false (scrolled away).
static void BackupCard(Ui::Id id, SDL_Rect r, const std::string& path, int i, int n, bool visible,
                       std::function<void()> press) {
    Focus::Node nd;
    nd.id = id; nd.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; nd.radius = 26; nd.flags = Focus::Ring;
    nd.hint = Tx("hint.open"); nd.press = std::move(press);
    Focus::Add(std::move(nd));
    if (!visible) return;
    W::BeginPress(id, r);
    Focus::DrawPlate(id);
    const bool f = Focus::Is(id);
    W::Card(r);
    if (f) FillRectRounded(r.x, r.y, r.w, r.h, 20, Alpha(gTheme.gold, 12));
    const BackupTs t = ParseBackupName(path);
    // The calendar leaf: the month on a coloured band, the day under it.
    const int lw = 66, lh = 76, lx = r.x + 16, ly = r.y + (r.h - lh) / 2;
    const SDL_Color band = f || i == 0 ? gTheme.gold : Mix(gTheme.gold, gTheme.panel, 0.45f);
    const SDL_Color bandInk = ColorLuma(band) > 150 ? SDL_Color{ 20, 18, 28, 255 } : SDL_Color{ 250, 250, 255, 255 };
    FillRectRounded(lx, ly + 4, lw, lh, 14, SDL_Color{ 0, 0, 0, 46 });
    FillRectRounded(lx, ly, lw, lh, 14, Mix(gTheme.panel, gTheme.text, 0.09f));
    FillRectRounded(lx, ly, lw, 26, 14, band);
    FillRectSquare(lx, ly + 13, lw, 13, band);
    if (t.ok) {
        DrawTextC(UpperUtf8(BackupMonShort(t.mon)), lx + lw / 2, ly + 13, bandInk, Font::Xs);
        DrawTextC(std::to_string(t.day), lx + lw / 2, ly + 26 + (lh - 26) / 2, gTheme.text, Font::Title);
    } else {
        UiIcons::Draw(UiIcons::Id::Clock, lx + lw / 2, ly + 26 + (lh - 26) / 2, 24, gTheme.dim);
    }
    // Right: the size, then Newest / Oldest.
    BackupService::BackupInfo bi;
    const bool have = BackupService::GetInfo(path, bi);
    const int rx = r.x + r.w - 20;
    int rightW = 0;
    if (have) {
        const std::string sz = SizeText(bi.bytes);
        rightW = TextW(sz, Font::Row);
        DrawTextR(sz, rx, r.y + 15 + TextH(Font::Row) / 2, f ? gTheme.text : Mix(gTheme.text, gTheme.dim, 0.2f), Font::Row);
    } else {
        W::Spinner(rx - 10, r.y + 15 + TextH(Font::Row) / 2, 9, gTheme.faint);
        rightW = 20;
    }
    std::string badge;
    if (i == 0) badge = Tx("restore.badge.newest");
    else if (i == n - 1) badge = Tx("restore.badge.oldest");
    if (!badge.empty()) {
        const int bw = W::PillW(badge);
        W::Pill(rx - bw, r.y + r.h - 16 - 28, badge, i == 0 ? Ui::Tone::Accent : Ui::Tone::Neutral);
        rightW = std::max(rightW, bw);
    }
    // Middle: when, then what is inside.
    const int tx = lx + lw + 20, tw = rx - rightW - 24 - tx;
    std::string tm;
    const std::string day = BackupWhen(path, &tm);
    const std::string age = BackupAge(t);
    const std::string line1 = age.empty() ? day : age;
    const std::string line2 = age.empty() ? tm : day + (tm.empty() ? "" : "  \xC2\xB7  " + tm);
    W::Marquee(id, line1, tx, r.y + 15, tw, f ? gTheme.text : Mix(gTheme.text, gTheme.dim, 0.2f), Font::Row, f);
    if (!line2.empty()) W::Marquee(Ui::IdMix(id, 0xDE5Cu), line2, tx, r.y + 15 + TextH(Font::Row), tw, gTheme.faint, Font::Body, f);
    const int iy = r.y + r.h - 25;
    if (!have) {
        DrawTextVC(Tx("bk.reading"), tx, iy, gTheme.faint, Font::Sm);
    } else {
        // As many as fit, in this order.
        struct Bit { Art::Id icon; std::string text; };
        std::vector<Bit> bits;
        // Long names are cut at rest and shown whole while the card is highlighted.
        if (!bi.island.empty()) bits.push_back({ Art::Id::IcoIsland, f ? bi.island : W::Ellipsize(bi.island, Font::Sm, 190) });
        if (!bi.player.empty()) bits.push_back({ Art::Id::IcoPlayer, f ? bi.player : W::Ellipsize(bi.player, Font::Sm, 160) });
        if (bi.miis >= 0) bits.push_back({ Art::Id::IcoMiis, CountText(bi.miis, "bk.mii1", "bk.miis") });
        bits.push_back({ Art::Id::IcoTextures, CountText(bi.textures, "bk.tex1", "bk.texs") });
        int x = tx;
        const SDL_Color c = f ? gTheme.dim : Mix(gTheme.dim, gTheme.faint, 0.35f);
        for (const Bit& b : bits) {
            const int need = 26 + TextW(b.text, Font::Sm);
            if (x + need > tx + tw + (badge.empty() ? rightW + 24 : 0)) break;
            x += InfoBit(x, iy, b.icon, b.text, c) + 18;
        }
    }
    W::EndPress();
}

// What a backup holds, in a dialog (its menu's "Details").
static void BackupDetailsDialog(Ui::Id id, const std::string& path, std::function<void()> close) {
    BackupService::BackupInfo bi;
    const bool have = BackupService::GetInfo(path, bi);
    std::string tm;
    std::string body = BackupWhen(path, &tm) + (tm.empty() ? "" : "  \xC2\xB7  " + tm);
    const BackupTs t = ParseBackupName(path);
    const std::string age = BackupAge(t);
    if (!age.empty()) body += "  (" + age + ")";
    if (have) {
        body += "\n" + SizeText(bi.bytes) + "  \xC2\xB7  " + CountText(bi.files, "bk.file1", "bk.files");
        std::string who;
        if (!bi.island.empty()) who = TokenFill(Tx("bk.d.island"), "{v}", bi.island);
        if (!bi.player.empty()) who += (who.empty() ? "" : "  \xC2\xB7  ") + TokenFill(Tx("bk.d.player"), "{v}", bi.player);
        if (!who.empty()) body += "\n" + who;
        std::string what = CountText(bi.textures, "bk.tex1", "bk.texs");
        if (bi.miis >= 0) what = CountText(bi.miis, "bk.mii1", "bk.miis") + "  \xC2\xB7  " + what;
        body += "\n" + what;
    } else {
        body += "\n" + Tx("bk.reading");
    }
    // The folder on two lines (a path has no spaces to wrap at).
    const size_t sl = path.rfind('/');
    if (sl != std::string::npos) body += "\n" + TokenFill(Tx("bk.d.folder"), "{v}", path.substr(0, sl + 1)) + "\n" + path.substr(sl + 1);
    else body += "\n" + TokenFill(Tx("bk.d.folder"), "{v}", path);
    W::Dialog(id, UiIcons::Id::Clock, Ui::Tone::Neutral, Tx("bk.details"), body,
              { { Tx("hint.close"), W::Kind::Primary, close } }, 0, close, Tx("hint.close"));
}

static void DoRestore(int idx) {
    const int u = gRestorePickerUserIdx;
    sBkAsk = 0; sBkMenu = -1;
    if (u < 0 || u >= (int)gUsers.size() || idx < 0 || idx >= (int)gRestoreList.size()) { CloseBackups(); return; }
    BusyFrame(Tx("restore.progress.title"), Tx("restore.progress.wait"));
    const std::string merr = SaveMount::Mount(gUsers[u].uid);
    if (!merr.empty()) {
        gShowRestorePicker = false;
        gRestoreFromHome = false;
        Sound::Play(Sound::Voice::Err);
        Ui::ToastShow(Tx("restore.toast.mount_fail") + " " + merr, Ui::Tone::Bad, 5.f);
        return;
    }
    const std::string rerr = BackupService::RestoreBackup(gRestoreList[idx], "tomodata:/");
    SaveMount::Commit();
    SaveMount::Unmount();
    gShowRestorePicker = false;
    Sound::Play(rerr.empty() ? Sound::Voice::Done : Sound::Voice::Err);
    if (gRestoreFromHome) {
        ReturnToEditorAfterRestore(u, rerr.empty() ? Tx("restore.toast.ok") : Tx("restore.toast.fail") + " " + rerr,
                                   rerr.empty() ? Ui::Tone::Good : Ui::Tone::Bad);
        return;
    }
    if (rerr.empty()) Ui::ToastShow(Tx("restore.toast.ok"), Ui::Tone::Good, 4.f);
    else              Ui::ToastShow(Tx("restore.toast.fail") + " " + rerr, Ui::Tone::Bad, 5.f);
}

static void DoDeleteBackup(int idx) {
    sBkAsk = 0; sBkMenu = -1;
    if (idx < 0 || idx >= (int)gRestoreList.size()) return;
    BackupService::ForgetInfo(gRestoreList[idx]);
    const bool ok = BackupService::DeleteBackupAt(gRestoreList[idx]);
    if (!ok) { Sound::Play(Sound::Voice::Err); return; }
    Sound::Play(Sound::Voice::Delete);
    gRestoreList = BackupService::ListBackups(gRestorePickerUid);
    RefreshBackupCounts(true);
    if (gRestoreList.empty() && gRestoreFromHome) {
        gShowRestorePicker = false;
        ReturnToEditorAfterRestore(gRestorePickerUserIdx, Tx("restore.toast.deleted_last"), Ui::Tone::Good);
        return;
    }
    Ui::ToastShow(Tx("restore.toast.deleted"), Ui::Tone::Good);
}

static Ui::Id BkRowId(int i) { return Ui::IdMix(Ui::HashStr("bk.row"), (uint32_t)i); }

static void BackupsSheet() {
    const Ui::Id id = Ui::HashStr("bk.sheet");
    const double since = Motion::FirstSeen(id);
    const double now = Motion::Now();
    if (now - since < 0.0001) { Sound::Play(Sound::Voice::Open); Focus::Set(BkRowId(0)); }
    const int u = gRestorePickerUserIdx;
    const std::string who = (u >= 0 && u < (int)gUsers.size()) ? gUsers[u].nickname : std::string();
    BackupService::RequestInfo(gRestoreList);
    // As tall as its list (up to four cards, then it scrolls).
    const int cardH = 112, step = cardH + 12;
    const int rowsShown = gRestoreList.empty() ? 2 : std::min(4, (int)gRestoreList.size());
    const int w = 820, h = std::max(330, 106 + rowsShown * step + 26), x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 10;
    Focus::PushLayer(Focus::Layer::Sheet, []() { CloseBackups(); }, Tx("hint.close"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(140 * std::min(1.f, (float)((now - since) / 0.16))));
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, std::min(1.f, (float)((now - since) / 0.42)));
    PushXform(0, 16.f * (1 - e), 0.98f + 0.02f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, e * 1.4f));
    FillRectRounded(x, y + 12, w, h, 28, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(x, y, w, h, 28, Mix(gTheme.panel, gTheme.text, 0.03f));
    DrawRectRounded(x, y, w, h, 28, Alpha(gTheme.border, 230));
    // Head
    W::IconTile(x + 28, y + 26, 56, UiIcons::Id::Clock, gTheme.gold);
    DrawText(Tx("backups.title"), x + 100, y + 26, gTheme.text, Font::Title);
    {
        std::string sub = TokenFill(TokenFill(Tx("backup.tip"), "{cur}", std::to_string(gRestoreList.size())), "{max}", std::to_string(gMaxBackups));
        if (!who.empty()) sub = who + "  \xC2\xB7  " + sub;
        W::Marquee(Ui::HashStr("restore.sub"), sub, x + 100, y + 26 + TextH(Font::Title), w - 220, gTheme.dim, Font::Body, true);
    }
    W::Button(Ui::HashStr("bk.close"), { x + w - 28 - 48, y + 30, 48, 48 }, std::string(), W::Kind::Neutral,
              []() { Sound::Play(Sound::Voice::Release); CloseBackups(); }, UiIcons::Id::Close, true, Focus::OwnSound);
    const SDL_Rect view{ x + 6, y + 100, w - 12, h - 100 - 8 };
    if (gRestoreList.empty()) {
        const int cy = view.y + view.h / 2 - 40;
        W::IconTile(SCREEN_W / 2 - 36, cy - 36, 72, UiIcons::Id::Clock, gTheme.dim);
        DrawTextC(Tx("restore.empty.title"), SCREEN_W / 2, cy + 64, gTheme.text, Font::Title);
        Ui::TextBlockC(Tx("restore.empty.hint"), SCREEN_W / 2, cy + 90, w - 120, gTheme.dim, Font::Body, 2);
    } else {
        static Focus::Scroll sc;
        Focus::BeginScroll(Ui::HashStr("bk.list"), view, sc);
        const int n = (int)gRestoreList.size(), pad = 12;
        for (int i = 0; i < n; i++) {
            const SDL_Rect r{ view.x + pad, view.y + pad + i * step - (int)sc.offset, view.w - 2 * pad, cardH };
            const bool vis = !(r.y + r.h < view.y || r.y > view.y + view.h);
            // Cards arrive one after another.
            const float rt = Motion::Reduced() ? 1.f : std::max(0.f, std::min(1.f, (float)((now - since - 0.08 - i * 0.05) / 0.4)));
            PushXform(0, 10.f * (1 - Motion::Ease(Motion::Curve::Spring, rt)), 1, 0, 0, std::min(1.f, rt * 1.5f));
            const int idx = i;
            BackupCard(BkRowId(i), r, gRestoreList[i], i, n, vis, [idx]() { sBkMenu = idx; Motion::Forget(Ui::HashStr("bk.menu")); });
            PopXform();
            if (i == n - 1 && rt > 0.f && rt < 0.08f) Sound::Play(Sound::Voice::Pluck, { 0.6f });
        }
        Focus::EndScroll((float)(2 * pad + n * step - 12));
    }
    PopXform();
    Focus::PopLayer();

    // A backup's menu, then the question before doing it.
    if (sBkMenu >= 0 && sBkMenu < (int)gRestoreList.size()) {
        const int b = sBkMenu;
        std::vector<W::MenuItem> items;
        items.push_back({ Tx("restore.menu.restore"), Tx("restore.confirm.restore.l1"), UiIcons::Id::Undo, false,
                          [b]() { sBkMenu = -1; sBkAsk = 1; sBkAskIdx = b; Motion::Forget(Ui::HashStr("bk.ask")); }, false });
        items.push_back({ Tx("bk.details"), Tx("bk.details.sub"), UiIcons::Id::Info, false,
                          [b]() { sBkMenu = -1; sBkAsk = 3; sBkAskIdx = b; Motion::Forget(Ui::HashStr("bk.info")); }, false });
        items.push_back({ Tx("restore.menu.delete"), Tx("restore.confirm.delete.l1"), UiIcons::Id::Trash, true,
                          [b]() { sBkMenu = -1; sBkAsk = 2; sBkAskIdx = b; Motion::Forget(Ui::HashStr("bk.ask")); }, false });
        W::Menu(Ui::HashStr("bk.menu"), BackupWhen(gRestoreList[b]), items, []() { sBkMenu = -1; }, Tx("hint.close"), 0,
                Focus::RectOf(BkRowId(b)));
    }
    if (sBkAsk == 3 && sBkAskIdx >= 0 && sBkAskIdx < (int)gRestoreList.size()) {
        const int b = sBkAskIdx;
        BackupDetailsDialog(Ui::HashStr("bk.info"), gRestoreList[b], [b]() { sBkAsk = 0; Focus::Set(BkRowId(b)); });
    } else if (sBkAsk && sBkAskIdx >= 0 && sBkAskIdx < (int)gRestoreList.size()) {
        std::string tm;
        const std::string when = BackupWhen(gRestoreList[sBkAskIdx], &tm) + (tm.empty() ? "" : "  \xC2\xB7  " + tm);
        const bool del = sBkAsk == 2;
        const int idx = sBkAskIdx;
        W::Dialog(Ui::HashStr("bk.ask"), del ? UiIcons::Id::Trash : UiIcons::Id::Undo, del ? Ui::Tone::Bad : Ui::Tone::Accent,
                  Tx(del ? "restore.confirm.delete.title" : "restore.confirm.restore.title"),
                  when + "\n" + Tx(del ? "restore.confirm.delete.l1" : "restore.confirm.restore.l1") + " " +
                      Tx(del ? "restore.confirm.delete.l2" : "restore.confirm.restore.l2"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sBkAsk = 0; } },
                    { Tx(del ? "restore.chip.delete" : "restore.chip.restore"), del ? W::Kind::Danger : W::Kind::Primary,
                      [idx, del]() { if (del) DoDeleteBackup(idx); else DoRestore(idx); } } },
                  0, []() { sBkAsk = 0; }, Tx("hint.cancel"));
    }
}

// ── Choose a user ────────────────────────────────────────────────────────────
static Ui::Id UserCardId(int i) { return Ui::IdMix(Ui::HashStr("start.user"), (uint32_t)i); }
static Ui::Id UserBkId(int i)   { return Ui::IdMix(Ui::HashStr("start.bk"), (uint32_t)i); }
static const Ui::Id kNewIslandId = Ui::HashStr("start.new");
static float sStartScroll = 0.f;

static void OpenUser(int i) {
    if (i < 0 || i >= (int)gUsers.size()) return;
    gUserSel = i;
    BusyFrame(Tx("mounting.title"));
    const std::string err = SaveMount::Mount(gUsers[i].uid);
    if (!err.empty()) {
        // No Tomodachi Life save for this user yet (2002-1002, whatever noise
        // sits in the top bits): offer to make one.
        if (Errors::IsSaveNotFound(err)) { gScreen = Screen::IslandGenPrompt; return; }
        gError = err;
        gScreen = Screen::Error;
        return;
    }
    // A container that was created but never finished seeding mounts, but
    // misses files the editor needs: make it again instead.
    struct stat st{};
    const bool partial = stat("tomodata:/Mii.sav", &st) != 0 || st.st_size == 0 ||
                         stat("tomodata:/Map.sav", &st) != 0 || st.st_size == 0 ||
                         stat("tomodata:/Player.sav", &st) != 0 || st.st_size == 0;
    if (partial) { SaveMount::Unmount(); gScreen = Screen::IslandGenPrompt; return; }
    gBackupFull = BackupService::CountBackups(SaveMount::SafeUserFolder(gUsers[i])) >= gMaxBackups;
    gScreen = Screen::BackupPrompt;
}

// The "+" card: a dashed outline with a plus.
static void NewIslandCard(SDL_Rect r, const std::string& label, std::function<void()> press) {
    Focus::Node n;
    n.id = kNewIslandId; n.r = { r.x - 8, r.y - 8, r.w + 16, r.h + 16 }; n.radius = 30; n.hint = Tx("hint.open");
    n.flags = Focus::Ring;
    n.press = std::move(press);
    Focus::Add(std::move(n));
    W::BeginPress(kNewIslandId, r);
    Focus::DrawPlate(kNewIslandId);
    const bool f = Focus::Is(kNewIslandId);
    FillRectRounded(r.x, r.y, r.w, r.h, 24, Alpha(gTheme.panel, f ? 200 : 110));
    // Dashes all the way round, corners included.
    DashRectRounded((float)r.x + 1, (float)r.y + 1, (float)r.w - 2, (float)r.h - 2, 23.f, 2.f, 9.f, 9.f,
                    f ? Alpha(gTheme.gold, 170) : Alpha(gTheme.text, 60));
    const int cy = r.y + 18 + (r.w - 36) / 2;
    FillDisc((float)(r.x + r.w / 2), (float)cy, 40.f, Alpha(gTheme.gold, f ? 70 : 30));
    UiIcons::Draw(UiIcons::Id::Plus, r.x + r.w / 2, cy, 40, f ? gTheme.gold : gTheme.dim);
    const int ny = r.y + 18 + (r.w - 36) + 18;
    W::Marquee(kNewIslandId, label, r.x + r.w / 2, ny + TextH(Font::Title) / 2, r.w - 24, f ? gTheme.text : gTheme.dim, Font::Title, f, W::Anchor::Centre);
    W::EndPress();
}

// A row of user cards that scrolls sideways when there are many.
// Returns the x of the first card. `withBackups` adds the backups button.
static void UserCards(int top, bool withNew, bool withBackups, std::function<void(int)> pick) {
    const int n = (int)gUsers.size();
    const int count = n + (withNew ? 1 : 0);
    const int cw = 216, gap = 28, ch = 18 + (cw - 36) + 18 + TextH(Font::Title) + 22;
    const int total = count * cw + (count - 1) * gap;
    // Keep the highlighted card on screen.
    int focusIdx = -1;
    for (int i = 0; i < n; i++) if (Focus::Is(UserCardId(i)) || Focus::Is(UserBkId(i))) focusIdx = i;
    if (Focus::Is(kNewIslandId)) focusIdx = n;
    float target = 0.f;
    if (total > SCREEN_W - 120) {
        const int startX = 60;
        if (focusIdx >= 0) {
            const int fx = startX + focusIdx * (cw + gap);
            target = std::max(0.f, std::min((float)(total - (SCREEN_W - 120)), (float)(fx + cw / 2 - SCREEN_W / 2)));
        } else target = sStartScroll;
    }
    sStartScroll += (target - sStartScroll) * (1.f - expf(-Motion::Dt() * 12.f));
    const int x0 = total > SCREEN_W - 120 ? 60 - (int)sStartScroll : (SCREEN_W - total) / 2;
    for (int i = 0; i < count; i++) {
        const SDL_Rect r{ x0 + i * (cw + gap), top, cw, ch };
        W::BeginBlock(r);
        if (i < n) {
            SDL_Texture* pic = i < (int)gAvatarTextures.size() ? gAvatarTextures[i] : nullptr;
            const int idx = i;
            W::PersonCard(UserCardId(i), r, pic, gUsers[i].nickname, std::string(), [pick, idx]() { pick(idx); },
                          i == std::max(0, std::min(gUserSel, n - 1)) ? Focus::Default : 0);
            if (withBackups) {
                RefreshBackupCounts(false);
                const int c = i < (int)sBkCounts.size() ? sBkCounts[i] : 0;
                W::Button(UserBkId(i), { r.x, r.y + ch + 18, cw, 46 }, BackupsLabel(c), W::Kind::Neutral,
                          [idx]() { OpenBackups(idx); }, UiIcons::Id::Clock);
            }
        } else {
            NewIslandCard(r, Tx("userpick.new_bundle"), []() { gBundleCreateSel = 0; gScreen = Screen::BundleCreate; });
        }
        W::EndBlock();
    }
    if (x0 < 40) UiIcons::Draw(UiIcons::Id::ChevronLeft, 24, top + ch / 2, 30, gTheme.dim);
    if (x0 + total > SCREEN_W - 40) UiIcons::Draw(UiIcons::Id::ChevronRight, SCREEN_W - 24, top + ch / 2, 30, gTheme.dim);
}

static void PageStart() {
    // A message from the last thing that happened (restore, new island).
    if (gBundleMsgFrames > 0) {
        const bool bad = gBundleMsgCol.r > 200 && gBundleMsgCol.g < 120;
        const bool good = gBundleMsgCol.g > 200 && gBundleMsgCol.r < 150;
        Ui::ToastShow(gBundleMsg, bad ? Ui::Tone::Bad : good ? Ui::Tone::Good : Ui::Tone::Neutral, 4.f);
        gBundleMsgFrames = 0;
    }
    TopBrand();
    W::Button(Ui::HashStr("start.settings"), { SCREEN_W - 40 - 186, 34, 186, 50 }, Tx("settings.title"), W::Kind::Neutral,
              []() { gSettingsMsg.clear(); gScreen = Screen::Settings; }, UiIcons::Id::Gear);
    W::BeginBlock({ 0, 120, SCREEN_W, 100 });
    DrawTextC(Tx("userpick.title"), SCREEN_W / 2, 150, gTheme.text, Font::Xl);
    DrawTextC(Tx("userpick.sub"), SCREEN_W / 2, 198, gTheme.dim, Font::Body);
    W::EndBlock();
    if (gUsers.empty()) {
        DrawTextC(Tx("error.title"), SCREEN_W / 2, 360, gTheme.dim, Font::Title);
    } else {
        UserCards(250, true, true, [](int i) { OpenUser(i); });
    }
    // Y: this user's backups.
    for (int i = 0; i < (int)gUsers.size(); i++) {
        if (Focus::Is(UserCardId(i))) {
            const int idx = i;
            Focus::Command(Input::Y, Tx("backups.title"), [idx]() { OpenBackups(idx); });
        }
    }
    Focus::Command(Input::Plus, Tx("quit.quit"), []() {});
    if (gShowRestorePicker) BackupsSheet();
}

// ── Back up first? ───────────────────────────────────────────────────────────
static void StartBackup() {
    const std::string uTag = (gUserSel >= 0 && gUserSel < (int)gUsers.size()) ? SaveMount::SafeUserFolder(gUsers[gUserSel]) : std::string();
    if (gBackupFull) BackupService::DeleteOldestBackup(uTag);
    BackupService::StartFullBackup("tomodata:/", uTag);
    gScreen = Screen::BackingUp;
}

static void PageBackupPrompt() {
    const std::string uTag = (gUserSel >= 0 && gUserSel < (int)gUsers.size()) ? SaveMount::SafeUserFolder(gUsers[gUserSel]) : std::string();
    static int sCount = 0;
    static double sCountAt = -10;
    if (Motion::Now() - sCountAt > 2.0) { sCount = BackupService::CountBackups(uTag); sCountAt = Motion::Now(); }
    const std::string cur = std::to_string(sCount), mx = std::to_string(gMaxBackups);
    auto fill2 = [&](const std::string& s) { return TokenFill(TokenFill(s, "{cur}", cur), "{max}", mx); };
    TopBrand();
    if (gUserSel >= 0 && gUserSel < (int)gUsers.size()) {
        const std::string who = gUsers[gUserSel].nickname;
        W::Pill(SCREEN_W - 40 - W::PillW(who, true), 44, who, Ui::Tone::Accent, true);
    }
    W::BeginBlock({ 0, 120, SCREEN_W, 100 });
    DrawTextC(gBackupFull ? fill2(Tx("backup.full.title")) : Tx("backup.title"), SCREEN_W / 2, 150, gTheme.text, Font::Xl);
    {
        const std::string tip = fill2(Tx("backup.tip"));
        W::Pill(SCREEN_W / 2 - W::PillW(tip) / 2, 186, tip, gBackupFull ? Ui::Tone::Warn : Ui::Tone::Neutral);
    }
    W::EndBlock();
    const int tw = 400, th = 244, gap = 32, ty = 256;
    const int x1 = SCREEN_W / 2 - gap / 2 - tw, x2 = SCREEN_W / 2 + gap / 2;
    W::BeginBlock({ x1, ty, tw, th });
    W::ChoiceTile(Ui::HashStr("bp.backup"), { x1, ty, tw, th }, UiIcons::Id::Floppy, gBackupFull ? gTheme.warn : gTheme.gold,
                  Tx(gBackupFull ? "backup.full.action" : "backup.action"),
                  Tx(gBackupFull ? "backup.full.hint" : "backup.action.hint"), []() { StartBackup(); }, Focus::Default);
    W::EndBlock();
    W::BeginBlock({ x2, ty, tw, th });
    W::ChoiceTile(Ui::HashStr("bp.skip"), { x2, ty, tw, th }, UiIcons::Id::ChevronRight, gTheme.accent, Tx("backup.skip"),
                  Tx("backup.skip.hint"), []() { gScreen = Screen::OnSwitch; });
    W::EndBlock();
    {
        const std::string hint = Tx("backup.progress.hint");
        const int maxW = SCREEN_W - 200;
        const int w = std::min(TextW(hint, Font::Body), maxW) + 30;
        UiIcons::Draw(UiIcons::Id::Info, SCREEN_W / 2 - w / 2 + 10, ty + th + 44, 20, gTheme.faint);
        W::Marquee(Ui::HashStr("bp.hint"), hint, SCREEN_W / 2 - w / 2 + 30, ty + th + 44, maxW, gTheme.faint, Font::Body, true, W::Anchor::Middle);
    }
    // B: back to the users (nothing was changed; the save closes again).
    Focus::SetBaseBack([]() {
        Sound::Play(Sound::Voice::Release);
        SaveMount::Unmount();
        gScreen = Screen::UserPick;
    }, Tx("hint.back"));
}

// ── Backing up ───────────────────────────────────────────────────────────────
static float sShownProgress = 0.f;
static void PageBackingUp() {
    if (BackupService::BackupDone()) {
        const std::string berr = BackupService::BackupError();
        if (gApplyBundleAfterBackup) {
            BackupService::Cleanup();
            const int idx = gApplyBundleUserIdx;
            gApplyBundleAfterBackup = false;
            gApplyBundleUserIdx = -1;
            BusyFrame(Tx("bundle.applying"));
            FinishBundleApply(idx);
        } else {
            BackupService::Cleanup();
            if (gBackupFromHome) {
                gBackupFromHome = false;
                gHomeCacheStale = true;
                if (berr.empty()) Ui::ToastShow(Tx("home.backup.done"), Ui::Tone::Good);
                else              Ui::ToastShow(Tx("home.backup.failed") + " " + berr, Ui::Tone::Bad, 5.f);
            } else if (!berr.empty()) {
                Ui::ToastShow(Tx("home.backup.failed") + " " + berr, Ui::Tone::Bad, 5.f);
            }
            gScreen = Screen::OnSwitch;
        }
        Sound::Play(berr.empty() ? Sound::Voice::Done : Sound::Voice::Err);
        sShownProgress = 0.f;
    }
    const float prog = BackupService::BackupProgress();
    sShownProgress += (prog - sShownProgress) * (1.f - expf(-Motion::Dt() * 10.f));
    const SDL_Rect c = MessageCard(680, 330);
    W::IconTile(SCREEN_W / 2 - 34, c.y + 34, 68, UiIcons::Id::Floppy, gTheme.gold);
    DrawTextC(Tx("backup.progress.title"), SCREEN_W / 2, c.y + 136, gTheme.text, Font::Title);
    W::ProgressBar({ c.x + 60, c.y + 176, c.w - 120, 14 }, sShownProgress);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", (int)(sShownProgress * 100 + 0.5f));
    DrawTextC(pct, SCREEN_W / 2, c.y + 222, gTheme.text, Font::Row);
    DrawTextC(Tx("backup.progress.warn"), SCREEN_W / 2, c.y + 262, gTheme.warn, Font::Body);
    Ui::TextBlockC(Tx("backup.progress.hint"), SCREEN_W / 2, c.y + 286, c.w - 80, gTheme.faint, Font::Xs, 2);
    W::EndBlock();
}

// ── Errors, updates, the applet warning ──────────────────────────────────────
static void PageError() {
    auto back = []() { gError.clear(); gScreen = Screen::UserPick; };
    // A known console error gets a name and what to do about it; the raw text
    // stays underneath, small, for a bug report.
    const Errors::Info info = Errors::Describe(gError);
    const bool known = !info.titleKey.empty();
    const std::string title = known ? Tx(info.titleKey.c_str()) : Tx("error.title");
    const std::string body = known ? Tx(info.bodyKey.c_str()) : gError;
    const auto lines = Ui::WrapLines(body, Font::Body, 600, 5);
    const auto detail = known ? Ui::WrapLines(gError, Font::Xs, 600, 2) : std::vector<std::string>();
    const int lh = TextH(Font::Body) + 4, dh = TextH(Font::Xs) + 2;
    const int h = 250 + (info.code.empty() ? 0 : 40) + (int)lines.size() * lh + (detail.empty() ? 0 : 14 + (int)detail.size() * dh);
    const SDL_Rect c = MessageCard(700, h);
    W::IconTile(SCREEN_W / 2 - 34, c.y + 34, 68, UiIcons::Id::Alert, known ? gTheme.warn : gTheme.red);
    W::Marquee(Ui::HashStr("err.title"), title, SCREEN_W / 2, c.y + 136, c.w - 60, gTheme.text, Font::Title, true, W::Anchor::Centre);
    int y = c.y + 162;
    if (!info.code.empty()) {
        const std::string code = TokenFill(Tx("error.code"), "{code}", info.code);
        W::Pill(SCREEN_W / 2 - W::PillW(code) / 2, y, code, known ? Ui::Tone::Warn : Ui::Tone::Bad);
        y += 40;
    }
    for (const auto& l : lines) { DrawTextC(l, SCREEN_W / 2, y + TextH(Font::Body) / 2, gTheme.dim, Font::Body); y += lh; }
    if (!detail.empty()) {
        y += 14;
        for (const auto& l : detail) { DrawTextC(l, SCREEN_W / 2, y + TextH(Font::Xs) / 2, gTheme.faint, Font::Xs); y += dh; }
    }
    W::Button(Ui::HashStr("err.back"), { SCREEN_W / 2 - 120, c.y + c.h - 30 - 56, 240, 56 }, Tx("hint.back"), W::Kind::Primary,
              back, UiIcons::Id::ChevronLeft);
    W::EndBlock();
    Focus::SetBaseBack([back]() { Sound::Play(Sound::Voice::Release); back(); }, Tx("hint.back"));
}

static void PageUpdateCheck() {
    const auto state = Updater::GetState();
    if (state == Updater::State::UpdateAvailable) gScreen = Screen::UpdateAvailable;
    else if (state == Updater::State::NoUpdate || state == Updater::State::Error || state == Updater::State::Idle)
        gScreen = Screen::UserPick;
    TopBrand();
    W::Spinner(SCREEN_W / 2, SCREEN_H / 2 - 50, 28, gTheme.gold);
    DrawTextC(Tx("update.checking"), SCREEN_W / 2, SCREEN_H / 2 + 14, gTheme.dim, Font::Title);
    W::Button(Ui::HashStr("upd.skip"), { SCREEN_W / 2 - 110, SCREEN_H / 2 + 70, 220, 54 }, Tx("update.skip"), W::Kind::Neutral,
              []() { gScreen = Screen::UserPick; });
    Focus::SetBaseBack([]() { gScreen = Screen::UserPick; }, Tx("update.skip"));
}

// A release's notes as plain lines: headings, bullets and links kept as
// words; GitHub's "by @someone in https://…" tails and the changelog link
// dropped.
static std::vector<std::string> NotesLines(const std::string& md, int maxLines) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= md.size() && (int)out.size() < maxLines) {
        size_t b = md.find('\n', a);
        if (b == std::string::npos) b = md.size();
        std::string l = md.substr(a, b - a);
        a = b + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t s = 0;
        while (s < l.size() && l[s] == ' ') s++;
        l = l.substr(s);
        if (l.empty() || l.rfind("**Full Changelog**", 0) == 0) continue;
        bool bullet = false;
        if (l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0) { l = l.substr(2); bullet = true; }
        while (!l.empty() && l[0] == '#') l.erase(0, 1);
        // [text](url) → text
        for (size_t p; (p = l.find("](")) != std::string::npos;) {
            const size_t open = l.rfind('[', p), close = l.find(')', p);
            if (open == std::string::npos || close == std::string::npos) break;
            l = l.substr(0, open) + l.substr(open + 1, p - open - 1) + l.substr(close + 1);
        }
        const size_t by = l.find(" by @");
        if (by != std::string::npos) l = l.substr(0, by);
        for (size_t p; (p = l.find("**")) != std::string::npos;) l.erase(p, 2);
        while (!l.empty() && l[0] == ' ') l.erase(0, 1);
        if (l.empty()) continue;
        out.push_back(bullet ? "\xE2\x80\xA2  " + l : l);
    }
    return out;
}

static void PageUpdateAvailable() {
    const Updater::State st = Updater::GetState();
    const bool dev = Updater::IsDevBuild();
    // The news: the game's studio on a screen at the top, with its title
    // and the new version on a ribbon; the notes read as the headlines.
    const bool art = Art::Has(Art::Id::NewsBg);
    const auto notes = NotesLines(Updater::GetNotes(), art ? 5 : 6);
    const int lh = TextH(Font::Body) + 2;
    const int notesH = notes.empty() ? 0 : 38 + (int)notes.size() * lh + 6;
    const int bannerH = 164;
    const int lift = art ? 14 + bannerH + 36 - 136 : 0;   // how much lower the title sits
    // What sits above the buttons (the download's bar, the ready note, the
    // development build's note or an error) gets its own room, so it never
    // runs into them.
    const int cardW = 760;
    const std::string idleNote = dev ? Tx("update.dev.note") : Updater::GetError();
    const int noteLines = idleNote.empty() ? 0 : (int)Ui::WrapLines(idleNote, Font::Body, cardW - 120, 2).size();
    const int aboveH = st == Updater::State::Downloading ? 76
                     : st == Updater::State::Ready ? 2 * lh + 26
                     : noteLines ? noteLines * lh + 22 : 0;
    const int h = std::max(300 + lift + notesH + 64, 216 + lift + notesH + aboveH + 100);
    const SDL_Rect c = MessageCard(cardW, h);
    const std::string to = "v" + Updater::GetLatestVersion();
    if (art) {
        int iw = 0, ih = 0;
        SDL_Texture* bg = Art::Get(Art::Id::NewsBg, &iw, &ih);
        const SDL_Rect b{ c.x + 14, c.y + 14, c.w - 28, bannerH };
        const int sh = std::min(ih, iw * b.h / std::max(1, b.w));
        const SDL_Rect src{ 0, std::max(0, (ih - sh) / 3), iw, sh };
        DrawTexture(bg, &src, b);
        for (int i = 0; i < 16; i++) FillRectSquare(b.x, b.y + b.h - 64 + i * 4, b.w, 4, SDL_Color{ 0, 0, 0, (Uint8)(i * 6) });
        MaskCorners(b.x, b.y, b.w, b.h, 20, Mix(gTheme.bg, gTheme.panel, 236 / 255.f));
        int lw = 0, lhh = 0;
        Art::Get(Art::Id::NewsLogo, &lw, &lhh);
        const int logoW = lw > 0 ? 92 : 0, logoH = lw > 0 ? 92 * lhh / lw : 0;
        const int rh = 40, rw = 18 + logoW + (logoW ? 14 : 0) + TextW(to, Font::Row) + 20;
        const int rx = b.x + 18, ry = b.y + b.h - 18 - rh;
        const SDL_Color ink = ColorLuma(gTheme.gold) > 150 ? SDL_Color{ 20, 18, 28, 255 } : SDL_Color{ 250, 250, 255, 255 };
        FillRectRounded(rx, ry + 4, rw, rh, rh / 2, SDL_Color{ 0, 0, 0, 90 });
        FillRectRounded(rx, ry, rw, rh, rh / 2, gTheme.gold);
        if (logoW) Art::Draw(Art::Id::NewsLogo, SDL_Rect{ rx + 18, ry + (rh - logoH) / 2, logoW, logoH }, ink);
        DrawTextVC(to, rx + 18 + logoW + (logoW ? 14 : 0), ry + rh / 2, ink, Font::Row);
    } else {
        W::IconTile(SCREEN_W / 2 - 34, c.y + 34, 68, UiIcons::Id::Download, gTheme.gold);
    }
    DrawTextC(Tx(st == Updater::State::Ready ? "update.ready.title" : "update.available"), SCREEN_W / 2, c.y + 136 + lift, gTheme.text, Font::Title);
    {
        const std::string from = dev ? Tx("update.devbuild") : std::string("v" APP_VERSION);
        const int fw = W::PillW(from), tw = W::PillW(to), aw = 34;
        const int px = SCREEN_W / 2 - (fw + aw + tw) / 2;
        W::Pill(px, c.y + 166 + lift, from, Ui::Tone::Neutral);
        UiIcons::Draw(UiIcons::Id::ChevronRight, px + fw + aw / 2, c.y + 180 + lift, 18, gTheme.dim);
        W::Pill(px + fw + aw, c.y + 166 + lift, to, Ui::Tone::Good, true);
    }
    int y = c.y + 216 + lift;
    if (!notes.empty()) {
        W::Eyebrow(c.x + 60, y, Tx("update.notes"), gTheme.faint);
        y += 28;
        for (size_t i = 0; i < notes.size(); i++) {
            // The first line, when it is not a bullet, is the headline.
            const bool head = i == 0 && notes[i].rfind("\xE2\x80\xA2", 0) != 0;
            W::Marquee(Ui::IdMix(Ui::HashStr("upd.note"), (uint32_t)i), notes[i], c.x + 60, y, c.w - 120, head ? gTheme.text : gTheme.dim, head ? Font::Row : Font::Body, true);
            y += head ? TextH(Font::Row) + 4 : lh;
        }
        y += 10;
    }
    // Where the update stands, and what can be done.
    const int by = c.y + c.h - 30 - 56, bw = 240, gap = 16;
    auto later = []() { if (Updater::GetState() == Updater::State::Downloading) Updater::CancelDownload(); gScreen = Screen::UserPick; };
    if (st == Updater::State::Downloading) {
        const float p = Updater::GetProgress();
        const double total = Updater::GetDownloadSize() / 1048576.0;
        W::ProgressBar({ c.x + 60, by - 66, c.w - 120, 10 }, p);
        DrawTextC(TokenFill(TokenFill(Tx("update.downloading"), "{done}", DecimalText(total * p, 1)), "{total}", DecimalText(total, 1)),
                  SCREEN_W / 2, by - 36, gTheme.dim, Font::Body);
        W::Button(Ui::HashStr("upd.cancel"), { SCREEN_W / 2 - bw / 2, by, bw, 56 }, Tx("picker.cancel"), W::Kind::Neutral,
                  []() { Updater::CancelDownload(); });
    } else if (st == Updater::State::Ready) {
        Ui::TextBlockC(Tx("update.ready.body"), SCREEN_W / 2, by - 78, c.w - 120, gTheme.dim, Font::Body, 2);
        W::Button(Ui::HashStr("upd.later"), { SCREEN_W / 2 - gap / 2 - bw, by, bw, 56 }, Tx("update.later"), W::Kind::Neutral, later);
        W::Button(Ui::HashStr("upd.restart"), { SCREEN_W / 2 + gap / 2, by, bw, 56 }, Tx("update.restart"), W::Kind::Primary,
                  []() { Sound::Play(Sound::Voice::Done); Updater::RequestRestart(); gQuitApp = true; }, UiIcons::Id::Count, true,
                  Focus::Default);
    } else {
        if (noteLines)
            Ui::TextBlockC(idleNote, SCREEN_W / 2, by - 16 - noteLines * lh, c.w - 120, dev ? gTheme.faint : gTheme.red, Font::Body, 2, 2);
        W::Button(Ui::HashStr("upd.later"), { SCREEN_W / 2 - gap / 2 - bw, by, bw, 56 }, Tx("update.later"), W::Kind::Neutral, later);
        W::Button(Ui::HashStr("upd.go"), { SCREEN_W / 2 + gap / 2, by, bw, 56 }, Tx("update.now"), W::Kind::Primary,
                  []() { Updater::StartDownload(); }, UiIcons::Id::Download, Updater::CanInstall(), Focus::Default);
    }
    W::EndBlock();
    Focus::SetBaseBack(later, Tx("update.later"));
}

static void PageApplet() {
    const SDL_Rect c = MessageCard(760, 460);
    W::IconTile(SCREEN_W / 2 - 34, c.y + 30, 68, UiIcons::Id::Alert, gTheme.warn);
    DrawTextC(Tx("applet.title"), SCREEN_W / 2, c.y + 128, gTheme.text, Font::Title);
    DrawTextC(Tx("applet.body.l1"), SCREEN_W / 2, c.y + 164, gTheme.dim, Font::Body);
    DrawTextC(Tx("applet.body.l2"), SCREEN_W / 2, c.y + 188, gTheme.dim, Font::Body);
    const int sw = 320, sh = 84, sg = 20, sy = c.y + 222;
    const int sxL = SCREEN_W / 2 - sg / 2 - sw, sxR = SCREEN_W / 2 + sg / 2;
    FillRectRounded(sxL, sy, sw, sh, 18, Mix(gTheme.panel, gTheme.red, 0.14f));
    UiIcons::Draw(UiIcons::Id::Close, sxL + 32, sy + sh / 2, 22, gTheme.red);
    DrawText(Tx("applet.wrong"), sxL + 60, sy + 14, gTheme.red, Font::Row);
    DrawText(Tx("applet.wrong.hint"), sxL + 60, sy + 16 + TextH(Font::Row), gTheme.dim, Font::Body);
    FillRectRounded(sxR, sy, sw, sh, 18, Mix(gTheme.panel, gTheme.green, 0.14f));
    UiIcons::Draw(UiIcons::Id::Check, sxR + 32, sy + sh / 2, 22, gTheme.green);
    DrawText(Tx("applet.right"), sxR + 60, sy + 14, gTheme.green, Font::Row);
    DrawText(Tx("applet.right.hint"), sxR + 60, sy + 16 + TextH(Font::Row), gTheme.text, Font::Body);
    Ui::TextBlockC(Tx("applet.note"), SCREEN_W / 2, sy + sh + 18, c.w - 80, gTheme.faint, Font::Body, 2);
    W::Button(Ui::HashStr("applet.exit"), { SCREEN_W / 2 - 120, c.y + c.h - 28 - 56, 240, 56 }, Tx("applet.exit.button"),
              W::Kind::Primary, []() { gQuitApp = true; }, UiIcons::Id::Exit);
    W::EndBlock();
    Focus::SetBaseBack([]() { gQuitApp = true; }, Tx("applet.exit.button"));
}

// ── Saved! ───────────────────────────────────────────────────────────────────
static double sSavedAt = -1;
static void PageSaved() {
    const double now = Motion::Now();
    if (sSavedAt < 0) { sSavedAt = now; Sound::Play(Sound::Voice::Done); }
    const float t = (float)(now - sSavedAt);
    const float pop = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Jelly, std::min(1.f, t / 0.6f));
    const int cy = SCREEN_H / 2 - 50;
    FillDisc(SCREEN_W / 2.f, (float)cy, 58.f * pop, Alpha(gTheme.green, 40));
    FillDisc(SCREEN_W / 2.f, (float)cy, 42.f * pop, gTheme.green);
    if (pop > 0.3f) UiIcons::Draw(UiIcons::Id::Check, SCREEN_W / 2, cy, (int)(46 * std::min(1.f, pop)), gTheme.bg);
    const float a = std::min(1.f, std::max(0.f, (t - 0.12f) / 0.3f));
    PushXform(0, 8.f * (1 - a), 1, 0, 0, a);
    DrawTextC(Tx("save.feedback.title"), SCREEN_W / 2, SCREEN_H / 2 + 34, gTheme.text, Font::Xl);
    DrawTextC(Tx("save.feedback.body"), SCREEN_W / 2, SCREEN_H / 2 + 80, gTheme.dim, Font::Body);
    PopXform();
    if (t > 1.5f) {
        sSavedAt = -1;
        if (gSaveFeedbackQuit) gQuitApp = true;
        else gScreen = Screen::UserPick;
    }
}

// ── No save yet: make one ────────────────────────────────────────────────────
static void CreateIslandHere() {
    SeedBlobs sb = LoadSeeds();
    if (!sb.ok()) { gError = Tx("islandgen.err_no_seed"); gScreen = Screen::Error; return; }
    BusyFrame(Tx("islandgen.creating"));
    const std::string err = SaveMount::CreateAndSeed(gUsers[gUserSel].uid, sb.mii.data(), sb.mii.size(), sb.map.data(),
                                                     sb.map.size(), sb.player.data(), sb.player.size());
    if (!err.empty()) { gError = err; gScreen = Screen::Error; return; }
    // The console's region and language, so the seed's own do not stick.
    const std::string lerr = PatchSeedLanguageInPlace("tomodata:/Player.sav");
    if (!lerr.empty()) LogERR("Region/language patch: " + lerr);
    if (FILE* f = fopen("tomodata:/.tomotool_first_run", "wb")) fclose(f);
    SaveMount::Commit();
    gBackupFull = BackupService::CountBackups(SaveMount::SafeUserFolder(gUsers[gUserSel])) >= gMaxBackups;
    Sound::Play(Sound::Voice::Done);
    gScreen = Screen::BackupPrompt;
}

static void PageNoSave() {
    TopBrand();
    const bool noSeed = seed_Mii_z_bin_size < 1024 || seed_Map_z_bin_size < 1024 || seed_Player_z_bin_size < 1024;
    const SDL_Rect c = MessageCard(700, noSeed ? 380 : 350);
    W::IconTile(SCREEN_W / 2 - 34, c.y + 34, 68, UiIcons::Id::Sparkle, gTheme.gold);
    DrawTextC(Tx("islandgen.title"), SCREEN_W / 2, c.y + 136, gTheme.text, Font::Title);
    int y = Ui::TextBlockC(Tx("islandgen.body"), SCREEN_W / 2, c.y + 160, c.w - 100, gTheme.text, Font::Body, 2);
    y = Ui::TextBlockC(Tx("islandgen.body2"), SCREEN_W / 2, y + 4, c.w - 100, gTheme.dim, Font::Body, 2);
    if (noSeed) Ui::TextBlockC(Tx("islandgen.no_seed"), SCREEN_W / 2, y + 6, c.w - 80, gTheme.red, Font::Xs, 2);
    const int bw = 260, gap = 16, by = c.y + c.h - 30 - 56;
    W::Button(Ui::HashStr("ns.back"), { SCREEN_W / 2 - gap / 2 - bw, by, bw, 56 }, Tx("hint.back"), W::Kind::Neutral,
              []() { gScreen = Screen::UserPick; });
    W::Button(Ui::HashStr("ns.create"), { SCREEN_W / 2 + gap / 2, by, bw, 56 }, Tx("islandgen.create"), W::Kind::Primary,
              []() { CreateIslandHere(); }, UiIcons::Id::Sparkle, !noSeed, Focus::Default);
    W::EndBlock();
    Focus::SetBaseBack([]() { Sound::Play(Sound::Voice::Release); gScreen = Screen::UserPick; }, Tx("hint.back"));
}

// ── New island: which kind, then for whom ────────────────────────────────────
static void CreateBundle(int kind) {
    SeedBlobs sb = LoadSeeds();
    if (!sb.ok()) { gError = Tx("islandgen.err_no_seed"); gScreen = Screen::Error; return; }
    gBundleCreateSel = kind;
    const time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char nameBuf[64];
    snprintf(nameBuf, sizeof nameBuf, "island_%04d%02d%02d_%02d%02d%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    BusyFrame(Tx("islandgen.creating"));
    const std::string werr = BackupService::WriteBundleFromSeed(nameBuf, sb.mii.data(), sb.mii.size(), sb.map.data(), sb.map.size(),
                                                                sb.player.data(), sb.player.size());
    if (!werr.empty()) { gError = werr; gScreen = Screen::Error; return; }
    {
        const std::string bundlePlayer = std::string(BUNDLE_ROOT) + "/" + nameBuf + "/Player.sav";
        const std::string lerr = PatchSeedLanguageInPlace(bundlePlayer);
        if (!lerr.empty()) LogERR("Region/language patch (bundle): " + lerr);
    }
    if (kind == 1) {
        const std::string sentinel = std::string(BUNDLE_ROOT) + "/" + nameBuf + "/.tomotool_first_run";
        if (FILE* sf = fopen(sentinel.c_str(), "wb")) fclose(sf);
    }
    gPendingBundleName = nameBuf;
    gPendingBundleGenerated = kind == 1;
    gBundleApplyUserSel = std::min(gUserSel, std::max(0, (int)gUsers.size() - 1));
    gShowBundleOverwriteConfirm = false;
    gScreen = Screen::BundlePick;
}

static void PageNewIsland() {
    TopBrand();
    W::BeginBlock({ 0, 110, SCREEN_W, 110 });
    DrawTextC(Tx("bundle.create.title"), SCREEN_W / 2, 140, gTheme.text, Font::Xl);
    Ui::TextBlockC(Tx("bundle.create.body"), SCREEN_W / 2, 172, 820, gTheme.dim, Font::Body, 2);
    W::EndBlock();
    const int tw = 420, th = 260, gap = 32, ty = 262;
    const int x1 = SCREEN_W / 2 - gap / 2 - tw, x2 = SCREEN_W / 2 + gap / 2;
    W::BeginBlock({ x1, ty, tw, th });
    W::ChoiceTile(Ui::HashStr("ni.blank"), { x1, ty, tw, th }, UiIcons::Id::Home, gTheme.gold, Tx("bundle.blank.title"),
                  Tx("bundle.blank.desc"), []() { CreateBundle(0); }, Focus::Default);
    W::EndBlock();
    W::BeginBlock({ x2, ty, tw, th });
    W::ChoiceTile(Ui::HashStr("ni.gen"), { x2, ty, tw, th }, UiIcons::Id::Sparkle, gTheme.accent, Tx("bundle.generated.title"),
                  Tx("bundle.generated.desc"), []() { CreateBundle(1); });
    W::EndBlock();
    Focus::SetBaseBack([]() { Sound::Play(Sound::Voice::Release); gScreen = Screen::UserPick; }, Tx("hint.back"));
}

static double sOverwriteAt = -1;
static void ApplyIslandTo(int u, bool confirmed) {
    if (u < 0 || u >= (int)gUsers.size()) return;
    gBundleApplyUserSel = u;
    if (!confirmed && SaveMount::SaveExists(gUsers[u].uid)) {
        gShowBundleOverwriteConfirm = true;
        sOverwriteAt = Motion::Now();
        Motion::Forget(Ui::HashStr("ai.ask"));
        return;
    }
    gShowBundleOverwriteConfirm = false;
    BusyFrame(Tx("bundle.applying"));
    const bool hadSave = SaveMount::SaveExists(gUsers[u].uid);
    if (hadSave) {
        const std::string err = SaveMount::Mount(gUsers[u].uid);
        if (!err.empty()) { gError = "Mount: " + err; gScreen = Screen::Error; return; }
        // The user's own save is copied to a backup before it is replaced.
        const std::string uTag = SaveMount::SafeUserFolder(gUsers[u]);
        if (BackupService::CountBackups(uTag) >= gMaxBackups) BackupService::DeleteOldestBackup(uTag);
        BackupService::StartFullBackup("tomodata:/", uTag);
        gApplyBundleAfterBackup = true;
        gApplyBundleUserIdx = u;
        gScreen = Screen::BackingUp;
        return;
    }
    FinishBundleApply(u);
}

static void PageApplyIsland() {
    TopBrand();
    if (!gPendingBundleName.empty()) {
        W::Pill(SCREEN_W - 40 - W::PillW(gPendingBundleName), 44, gPendingBundleName, Ui::Tone::Neutral);
    }
    W::BeginBlock({ 0, 120, SCREEN_W, 100 });
    DrawTextC(Tx("bundle.apply.title"), SCREEN_W / 2, 150, gTheme.text, Font::Xl);
    DrawTextC(Tx("bundle.apply.overwrite.l2"), SCREEN_W / 2, 198, gTheme.dim, Font::Body);
    W::EndBlock();
    auto cancel = []() {
        if (!gPendingBundleName.empty()) BackupService::DeleteBundleByName(gPendingBundleName);
        gPendingBundleName.clear();
        gShowBundleOverwriteConfirm = false;
        Ui::ToastShow(Tx("bundle.dismissed"), Ui::Tone::Neutral);
        gScreen = Screen::UserPick;
    };
    if (gUsers.empty()) { cancel(); return; }
    UserCards(250, false, false, [](int i) { ApplyIslandTo(i, false); });
    Focus::SetBaseBack([cancel]() { Sound::Play(Sound::Voice::Release); cancel(); }, Tx("hint.cancel"));
    if (gShowBundleOverwriteConfirm) {
        const int u = gBundleApplyUserSel;
        const std::string nick = (u >= 0 && u < (int)gUsers.size()) ? gUsers[u].nickname : std::string();
        const double left = 2.0 - (Motion::Now() - sOverwriteAt);
        std::string yes = Tx("bundle.apply.overwrite.yes");
        if (left > 0) yes += "  (" + std::to_string((int)ceil(left)) + ")";
        W::Dialog(Ui::HashStr("ai.ask"), UiIcons::Id::Alert, Ui::Tone::Bad, Tx("bundle.apply.overwrite.title"),
                  nick + "\n" + Tx("bundle.apply.overwrite.l1") + " " + Tx("bundle.apply.overwrite.l2"),
                  { { Tx("bundle.apply.overwrite.no"), W::Kind::Neutral, []() { gShowBundleOverwriteConfirm = false; } },
                    { yes, W::Kind::Danger, [u]() { ApplyIslandTo(u, true); }, left <= 0 } },
                  0, []() { gShowBundleOverwriteConfirm = false; }, Tx("hint.cancel"));
    }
}

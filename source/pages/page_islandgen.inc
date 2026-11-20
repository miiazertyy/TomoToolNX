// page_islandgen.inc — TomoToolNX 2.0: the on-Switch island generator. Its
// choices, the optional "bring your own Miis over USB" step, and the run.
// The generator itself is 1.x's (IslandGenRunner). Included by shell.inc.

static void StartGenerator(bool batchLtd) {
    if (MtpServer::IsRunning() && batchLtd) MtpServer::Exit();
    IslandGenRunner::Options opts;
    opts.mapMode = gIslandGenMapMode;
    opts.mapId = std::string();
    opts.relMode = gIslandGenRelMode;
    opts.batchLtdMiis = batchLtd;
    gIslandGenProgress = IslandGenRunner::Progress{};
    if (!IslandGenRunner::Start(opts)) { gError = "Generator already running."; gScreen = Screen::Error; return; }
    gScreen = Screen::IslandGenRun;
}

static void PageIslandGen() {
    TopBrand();
    const int w = 820, x = (SCREEN_W - w) / 2;
    W::BeginBlock({ x, 100, w, 90 });
    const int hy = W::PageHeader(x, 100, w, UiIcons::Id::Sparkle, Tx("islandgen.cfg.header"), Tx("islandgen.cfg.title"), Tx("islandgen.cfg.body"));
    W::EndBlock();
    const SDL_Rect card{ x, hy, w, 3 * W::kRowH + 2 + 12 };
    W::BeginBlock(card);
    W::Card(card);
    int ry = hy + 6;
    const bool random = gIslandGenMapMode == "random";
    W::RowStepper(Ui::HashStr("ig.map"), { x + 6, ry, w - 12, W::kRowH }, { Tx("islandgen.cfg.row.map"), Tx("islandgen.cfg.row.map.desc") },
                  IslandGenMapModeLabel(), random ? 1.f : 0.f, 1.f, [](int d) { IslandGenCycleMap(d); });
    ry += W::kRowH; W::Hairline(x + 24, ry, w - 48); ry += 1;
    const bool dense = gIslandGenRelMode == "dense";
    W::RowStepper(Ui::HashStr("ig.rels"), { x + 6, ry, w - 12, W::kRowH }, { Tx("islandgen.cfg.row.rels"), Tx("islandgen.cfg.row.rels.desc") },
                  Tx(dense ? "islandgen.cfg.rels.dense" : "islandgen.cfg.rels.none"), dense ? 1.f : 0.f, 1.f,
                  [](int d) { gIslandGenRelMode = d > 0 ? "dense" : "none"; });
    ry += W::kRowH; W::Hairline(x + 24, ry, w - 48); ry += 1;
    W::RowToggle(Ui::HashStr("ig.batch"), { x + 6, ry, w - 12, W::kRowH }, { Tx("islandgen.cfg.row.batch"), Tx("islandgen.cfg.row.batch.desc") },
                 gIslandGenBatchLtd, [](bool v) { gIslandGenBatchLtd = v; });
    W::EndBlock();
    const int by = card.y + card.h + 28, bw = 260;
    W::Button(Ui::HashStr("ig.skip"), { SCREEN_W / 2 - 8 - bw, by, bw, 56 }, Tx("islandgen.cfg.skip"), W::Kind::Neutral,
              []() { gScreen = Screen::OnSwitch; });
    W::Button(Ui::HashStr("ig.go"), { SCREEN_W / 2 + 8, by, bw, 56 }, Tx("islandgen.cfg.go"), W::Kind::Primary, []() {
        if (gIslandGenBatchLtd) { gScreen = Screen::IslandGenBatchLtd; return; }
        StartGenerator(false);
    }, UiIcons::Id::Sparkle, true, Focus::Default);
    Focus::SetBaseBack([]() { Sound::Play(Sound::Voice::Release); gScreen = Screen::OnSwitch; }, Tx("islandgen.cfg.skip"));
}

static void PageIslandGenBatch() {
    TopBrand();
    mkdir("/switch/TomoToolNX", 0777);
    mkdir("/switch/TomoToolNX/Miis", 0777);
    const SDL_Rect c = MessageCard(760, 420);
    W::IconTile(SCREEN_W / 2 - 34, c.y + 30, 68, UiIcons::Id::Usb, gTheme.gold);
    DrawTextC(Tx("islandgen.batchltd.title"), SCREEN_W / 2, c.y + 128, gTheme.text, Font::Title);
    int y = Ui::TextBlockC(Tx("islandgen.batchltd.body1"), SCREEN_W / 2, c.y + 154, c.w - 80, gTheme.text, Font::Body, 2);
    y = Ui::TextBlockC(Tx("islandgen.batchltd.body2"), SCREEN_W / 2, y + 2, c.w - 80, gTheme.dim, Font::Body, 2);
    const std::string folder = "/switch/TomoToolNX/Miis/";
    const int fw = TextW(folder, Font::Row) + 48;
    FillRectRounded(SCREEN_W / 2 - fw / 2, y + 10, fw, 46, 14, Alpha(gTheme.bg, 160));
    DrawTextC(folder, SCREEN_W / 2, y + 33, gTheme.gold, Font::Row);
    const bool mr = MtpServer::IsRunning(), mo = MtpServer::IsSessionOpen();
    const std::string st = Tx("home.usb.title") + ":  " + Tx(mo ? "home.usb.on" : mr ? "home.usb.wait" : "home.usb.off");
    W::Pill(SCREEN_W / 2 - W::PillW(st, true) / 2, y + 70, st, mo ? Ui::Tone::Good : mr ? Ui::Tone::Warn : Ui::Tone::Neutral, true);
    const int bw = 250, by = c.y + c.h - 28 - 56;
    W::Button(Ui::HashStr("igb.usb"), { SCREEN_W / 2 - 8 - bw, by, bw, 56 }, Tx(mr ? "islandgen.batchltd.mtp_off" : "islandgen.batchltd.mtp_on"),
              W::Kind::Neutral, []() { TToggleMtp(0); }, UiIcons::Id::Usb);
    W::Button(Ui::HashStr("igb.go"), { SCREEN_W / 2 + 8, by, bw, 56 }, Tx("islandgen.batchltd.go"), W::Kind::Primary,
              []() { StartGenerator(true); }, UiIcons::Id::Sparkle, true, Focus::Default);
    W::EndBlock();
    Focus::SetBaseBack([]() { Sound::Play(Sound::Voice::Release); gScreen = Screen::IslandGenConfig; }, Tx("hint.back"));
    if (gShowMtpWarning) MtpDialog();
}

static void PageIslandGenRun() {
    gIslandGenProgress = IslandGenRunner::Snapshot();
    const IslandGenRunner::Progress& p = gIslandGenProgress;
    static float sShown = 0.f;
    static bool sWasDone = false;
    if (p.step == 0 && !p.done) sShown = 0.f;
    const int total = std::max(1, p.total);
    float target = (float)p.step / total;
    if (p.done && p.error.empty() && !p.cancelled) target = 1.f;
    sShown += (target - sShown) * (1.f - expf(-Motion::Dt() * 4.f));
    if (p.done && !sWasDone) Sound::Play(p.error.empty() && !p.cancelled ? Sound::Voice::Done : Sound::Voice::Err);
    sWasDone = p.done;
    const SDL_Rect c = MessageCard(720, 340);
    W::IconTile(SCREEN_W / 2 - 34, c.y + 30, 68, UiIcons::Id::Sparkle, p.done && !p.error.empty() ? gTheme.red : gTheme.gold);
    DrawTextC(Tx("islandgen.run.title"), SCREEN_W / 2, c.y + 128, gTheme.text, Font::Title);
    const std::string label = !p.error.empty() ? p.error : (p.label.empty() ? Tx("islandgen.run.starting") : p.label);
    W::Marquee(Ui::HashStr("igen.label"), label, SCREEN_W / 2, c.y + 164, c.w - 80, !p.error.empty() ? gTheme.red : gTheme.dim, Font::Body, true, W::Anchor::Centre);
    W::ProgressBar({ c.x + 60, c.y + 192, c.w - 120, 14 }, sShown);
    char buf[32];
    snprintf(buf, sizeof buf, "%d / %d", std::min(p.step, total), total);
    DrawTextC(buf, SCREEN_W / 2, c.y + 230, gTheme.faint, Font::Body);
    const int bw = 260, by = c.y + c.h - 26 - 56;
    if (!p.done) {
        W::Button(Ui::HashStr("igr.cancel"), { SCREEN_W / 2 - bw / 2, by, bw, 56 }, Tx("islandgen.run.cancel"), W::Kind::Neutral,
                  []() { IslandGenRunner::RequestCancel(); });
        Focus::SetBaseBack([]() { IslandGenRunner::RequestCancel(); }, Tx("islandgen.run.cancel"));
    } else {
        auto finish = []() {
            const IslandGenRunner::Progress& q = gIslandGenProgress;
            IslandGenRunner::Cleanup();
            if (!q.error.empty()) { gError = "Island generator: " + q.error; gScreen = Screen::Error; return; }
            // The generator rewrote the saves on disk: start from them.
            MiiManager::LoadUgcNames();
            gMiis = MiiManager::ListMiis();
            { gEntries = UgcScanner::Scan(SAVE_UGC_PATH); gUgcGen++; }
            gUgcFilter.clear();
            RebuildUgcFilter();
            gEntrySel = 0; gEntryScroll = 0;
            gPlayerSav = SaveEditor::SavFile{};
            gMiiSav = SaveEditor::SavFile{};
            gPlayerSavDirty = false;
            gMiiSavDirty = false;
            gScreen = Screen::OnSwitch;
        };
        W::Button(Ui::HashStr("igr.done"), { SCREEN_W / 2 - bw / 2, by, bw, 56 },
                  Tx(!p.error.empty() ? "islandgen.run.dismiss_error" : p.cancelled ? "islandgen.run.dismiss_cancel" : "islandgen.run.dismiss_done"),
                  W::Kind::Primary, finish, UiIcons::Id::Check, true, Focus::Default);
        Focus::SetBaseBack(finish, Tx("hint.close"));
    }
    W::EndBlock();
}

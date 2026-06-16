# PocketDisplay — Roadmap to Commercial Release

Status: core features working (connection, UI/display, cursors). PR #21–#24 merged to master.
Goal: ship a sellable Windows-to-Android second-display app.

> ✅ **x264 GPL blocker resolved** — Media Foundation H.264 encoder is now the default (non-GPL). x264 is optional via `POCKETDISPLAY_ENABLE_X264` CMake option.

---

## 0. BLOCKERS — must resolve before selling

- [x] **x264 encoder is GPL-licensed.** ✅ RESOLVED — Gated behind `POCKETDISPLAY_ENABLE_X264` (default OFF). Media Foundation H.264 encoder is now the default with hardware → software MFT fallback. For commercial builds, ensure POCKETDISPLAY_ENABLE_X264=OFF.
- [ ] **Full dependency license audit.** Check EVERY bundled library (Windows +
      Android) for GPL/LGPL/CC-BY-SA/NC. Cursor assets are already CC0 (safe).

---

## 1. Technical / functional

- [ ] Resize cursor detection in apps with custom cursors (e.g. Claude desktop)
      — Windows side only matches standard IDC_SIZE* handles. (Deferred item.)
- [ ] VDD extended display (#20) — true borderless extended display, resolution
      matched to the phone.
- [ ] Cross-device testing: multiple Android phones (resolutions/aspect
      ratios), multiple Windows GPUs/monitors.
- [ ] Stability: long sessions, repeated reconnects, multi-resolution switches.
- [ ] Performance pass: latency, framerate, bandwidth — USB vs WiFi.

## 2. Distribution / packaging

- [ ] Proper **Windows installer** (.exe / .msi), not just a raw binary.
- [ ] **Windows code signing** (cert) — without it, SmartScreen/Defender warns
      "unknown publisher," which scares buyers.
- [ ] **APK signing** + distribution channel decision:
      - Google Play (review process, $25 dev account, privacy policy required),
        or
      - Direct sideload / own site.
- [ ] Auto-update mechanism (optional but nice).

## 3. Legal

- [x] Resolve x264 (see Blockers — RESOLVED).
- [ ] Complete **THIRD_PARTY_LICENSES** for ALL dependencies (not just cursors).
- [ ] **EULA / Terms of Service.**
- [ ] **Privacy Policy** (required for Play Store; good practice anyway).

## 4. Commercial

- [ ] Pricing model (one-time / subscription / freemium).
- [ ] License-key / activation system (anti-piracy).
- [ ] Payment processor (Gumroad, Paddle, Stripe, …).
- [ ] Landing page / website.

## 5. Product polish

- [ ] Branding: final name, logo, app icon.
- [ ] First-run onboarding / tutorial.
- [ ] User-friendly error handling (no raw crashes/technical logs).
- [ ] User-facing help / docs.

---

## Suggested order

1. ~~**Resolve x264 license**~~ ✅ COMPLETED — Media Foundation encoder is now default.
2. Full dependency license audit.
3. Stability + cross-device testing.
4. Packaging: installer + code signing (Windows), APK signing.
5. Legal docs: EULA, privacy policy, licenses file.
6. Commercial: pricing, activation, payments, landing page.
7. Polish: branding, onboarding, error handling.
8. (Anytime) deferred features: resize detection, VDD extended display.

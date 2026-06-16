# PocketDisplay — Roadmap to Commercial Release

Status: core features working (connection, UI/display, cursors). PR #21–#24 merged to master.
Goal: ship a sellable Windows-to-Android second-display app.

> ⚠️ Read the **Legal / Licensing** section first. There is at least one
> blocking issue (x264 is GPL) that must be resolved before selling.

---

## 0. BLOCKERS — must resolve before selling

- [ ] **x264 encoder is GPL-licensed.** It's a core component (encodes the
      captured screen to H.264 before streaming). GPL is copyleft: shipping a
      closed-source commercial app linked against x264 can force the whole app
      to be GPL (must open-source + allow free redistribution). Options:
      - Buy a **commercial x264 license** (via x264's commercial licensing), OR
      - Switch encoder to a non-GPL one:
        - **Windows Media Foundation H.264** (built-in, no GPL)
        - **NVENC** (NVIDIA), **AMF** (AMD), **QuickSync** (Intel) — hardware
          encoders, usually commercial-friendly, faster + lower CPU
      - Note: separate from GPL, **H.264 itself has patent/royalty terms
        (MPEG-LA)** — check thresholds for a paid product.
      - ⚠️ Not legal advice — confirm with someone who knows software licensing.
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

- [ ] Resolve x264 (see Blockers).
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

1. **Resolve x264 license** (blocker — do this first; it may change the encoder
   code and everything downstream).
2. Full dependency license audit.
3. Stability + cross-device testing.
4. Packaging: installer + code signing (Windows), APK signing.
5. Legal docs: EULA, privacy policy, licenses file.
6. Commercial: pricing, activation, payments, landing page.
7. Polish: branding, onboarding, error handling.
8. (Anytime) deferred features: resize detection, VDD extended display.

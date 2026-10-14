"""mtrxAI GUI design system — the #26ab07-derived palette as code.

Single source of truth for both themes. The accent anchor is #26ab07 (hue ≈109°,
"matrix green"); every other color is derived from that hue so the whole UI
speaks one language:

  - ACCENT_RAMP   50→950 ramp anchored on #26ab07 (theme-independent)
  - LIGHT / DARK  semantic tokens for each theme (green-tinted neutrals)

`theme_css()` renders the token blocks served as /theme.css; `contrast()`
exposes the WCAG math so tests can prove the critical pairs pass.
"""

from __future__ import annotations

# The anchor. Never re-derive it — this IS the brand value.
ACCENT = "#26ab07"

# hue 109°, saturation tuned per stop so light stops stay lively, not minty.
ACCENT_RAMP: dict[str, str] = {
    "accent-050": "#effae9",
    "accent-100": "#ddf5d1",
    "accent-200": "#bce9a6",
    "accent-300": "#8fd96a",
    "accent-400": "#63ca3d",
    "accent-500": "#3cb61a",
    "accent-600": ACCENT,       # #26ab07 — the identity value
    "accent-700": "#1d8506",    # filled-button stop on light (white text AA)
    "accent-800": "#16680a",    # accent-as-text on light backgrounds
    "accent-900": "#12540b",
    "accent-950": "#0a3004",
}

# Semantic tokens. Keys become --<key> CSS custom properties.
# Contrast contract (verified by tests/test_gui.py):
#   *-text tokens    ≥ 4.5:1 on --bg / --surface  (WCAG 1.4.3 body text)
#   --on-accent      ≥ 4.5:1 on --accent-strong   (button labels)
#   --border-strong  ≥ 3:1 on --bg                (WCAG 1.4.11 — control boundaries:
#                                                 inputs, active borders, focus)
#   --border         ≥ 1.5:1 on --bg              (decorative dividers/cards; the
#                                                 3:1 rule does not apply to these)
LIGHT: dict[str, str] = {
    "bg":             "#f2f4ee",
    "surface":        "#fafbf7",
    "surface-2":      "#ffffff",
    "hover":          "#e9eee2",
    "border":         "#c0cab0",
    "border-strong":  "#6e7d5f",
    "text":           "#1d2317",
    "text-muted":     "#57624b",
    "text-faint":     "#7d8871",
    "accent":         ACCENT,
    "accent-text":    "#16680a",
    "accent-strong":  "#1d8506",
    "on-accent":      "#ffffff",
    "accent-soft":    "#e4f4dc",
    "focus":          "#16680a",
    "success":        "#26ab07",
    "success-text":   "#16680a",
    "success-soft":   "#e4f4dc",
    "error":          "#c62f21",
    "error-text":     "#b3271b",
    "error-soft":     "#fbe9e6",
    "warning":        "#b87700",
    "warning-text":   "#8a5a00",
    "warning-soft":   "#fdf2dc",
    "info":           "#0e6f8a",
    "info-text":      "#0e6f8a",
    "info-soft":      "#e3f2f7",
    "shadow":         "rgba(23, 42, 16, 0.12)",
    "scrim":          "rgba(29, 35, 23, 0.45)",
}

DARK: dict[str, str] = {
    "bg":             "#0f120d",
    "surface":        "#161b12",
    "surface-2":      "#1b2216",
    "hover":          "#222a1c",
    "border":         "#323e28",
    "border-strong":  "#5c6e50",
    "text":           "#e8ecdf",
    "text-muted":     "#a3b193",
    "text-faint":     "#7a8870",
    "accent":         ACCENT,
    "accent-text":    "#63ca3d",
    "accent-strong":  "#3cb61a",
    "on-accent":      "#0b1a05",
    "accent-soft":    "rgba(38, 171, 7, 0.14)",
    "focus":          "#8fd96a",
    "success":        "#3cb61a",
    "success-text":   "#63ca3d",
    "success-soft":   "rgba(38, 171, 7, 0.14)",
    "error":          "#ff7a6b",
    "error-text":     "#ff7a6b",
    "error-soft":     "rgba(255, 90, 74, 0.13)",
    "warning":        "#ffb224",
    "warning-text":   "#ffb224",
    "warning-soft":   "rgba(255, 178, 36, 0.12)",
    "info":           "#4cc2e0",
    "info-text":      "#4cc2e0",
    "info-soft":      "rgba(76, 194, 224, 0.12)",
    "shadow":         "rgba(0, 0, 0, 0.45)",
    "scrim":          "rgba(0, 0, 0, 0.60)",
}

# Pairs the tests must prove (fg, bg, minimum) per theme — the AA contract.
_CONTRACT = {
    "text": 4.5,
    "text-muted": 4.5,
    "accent-text": 4.5,
    "success-text": 4.5,
    "error-text": 4.5,
    "warning-text": 4.5,
    "info-text": 4.5,
    "border-strong": 3.0,
    "border": 1.5,
}


# ---------------------------------------------------------------- color math

def _channel_lin(v: int) -> float:
    v /= 255
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def relative_luminance(hex_color: str) -> float:
    h = hex_color.lstrip("#")
    r, g, b = (int(h[i : i + 2], 16) for i in (0, 2, 4))
    return 0.2126 * _channel_lin(r) + 0.7152 * _channel_lin(g) + 0.0722 * _channel_lin(b)


def contrast(fg: str, bg: str) -> float:
    """WCAG 2.x contrast ratio between two hex colors."""
    l1, l2 = sorted((relative_luminance(fg), relative_luminance(bg)), reverse=True)
    return (l1 + 0.05) / (l2 + 0.05)


def contrast_report() -> list[tuple[str, str, float, float]]:
    """(token, theme, ratio, minimum) for every contracted pair — test + docs aid."""
    rows = []
    for theme_name, tokens in (("light", LIGHT), ("dark", DARK)):
        for key, minimum in _CONTRACT.items():
            rows.append((key, theme_name, contrast(tokens[key], tokens["bg"]), minimum))
        rows.append(("text", "light/surface", contrast(LIGHT["text"], LIGHT["surface"]), 4.5))
        rows.append(("text", "dark/surface", contrast(DARK["text"], DARK["surface"]), 4.5))
        rows.append(("on-accent", "light/strong", contrast(LIGHT["on-accent"], LIGHT["accent-strong"]), 4.5))
        rows.append(("on-accent", "dark/strong", contrast(DARK["on-accent"], DARK["accent-strong"]), 4.5))
    return rows


# ------------------------------------------------------------------- css out

def _block(tokens: dict[str, str]) -> str:
    return "".join(f"  --{k}: {v};\n" for k, v in tokens.items())


def theme_css() -> str:
    """Both themes as CSS custom properties: light default + dark override."""
    ramp = _block(ACCENT_RAMP)
    return (
        "/* Generated by mtrxai.gui.theme — edit the Python, not this file. */\n"
        f":root {{\n{ramp}{_block(LIGHT)}}}\n"
        f"[data-theme='dark'] {{\n{_block(DARK)}}}\n"
    )

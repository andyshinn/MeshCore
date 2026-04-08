#!/usr/bin/env python3
"""
Austin Mesh — Radio Info Card Generator

Generates a single static 4×6 thermal shipping label PDF for Meshcore LoRa radios.

Usage:
    python generate_card.py
    python generate_card.py --out cards/radio_card.pdf
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Optional, Sequence

from reportlab.lib.units import inch
from reportlab.lib.colors import black
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfgen import canvas
from reportlab.platypus import Paragraph
from reportlab.graphics.barcode.qr import QrCodeWidget
from reportlab.graphics import renderPDF
from reportlab.graphics.shapes import Drawing
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

# ── Font registration ────────────────────────────────────────────
_FONT_DIR = Path(__file__).parent / "fonts"
pdfmetrics.registerFont(TTFont("Ubuntu", _FONT_DIR / "Ubuntu/Ubuntu-Regular.ttf"))
pdfmetrics.registerFont(TTFont("Ubuntu-Bold", _FONT_DIR / "Ubuntu/Ubuntu-Bold.ttf"))
pdfmetrics.registerFont(TTFont("Ubuntu-Italic", _FONT_DIR / "Ubuntu/Ubuntu-Italic.ttf"))
pdfmetrics.registerFont(TTFont("Ubuntu-BoldItalic", _FONT_DIR / "Ubuntu/Ubuntu-BoldItalic.ttf"))
pdfmetrics.registerFont(TTFont("UbuntuMono", _FONT_DIR / "Ubuntu_Mono/UbuntuMono-Regular.ttf"))
pdfmetrics.registerFont(TTFont("UbuntuMono-Bold", _FONT_DIR / "Ubuntu_Mono/UbuntuMono-Bold.ttf"))

pdfmetrics.registerFontFamily(
    "Ubuntu",
    normal="Ubuntu",
    bold="Ubuntu-Bold",
    italic="Ubuntu-Italic",
    boldItalic="Ubuntu-BoldItalic",
)

# ── Page geometry ────────────────────────────────────────────────
PAGE_W = 4.01 * inch   # 288 pt
PAGE_H = 6 * inch   # 432 pt
MARGIN = 0.25 * inch
CONTENT_W = PAGE_W - 2 * MARGIN

INFO_URL = "https://austinmesh.org/radio"
DEFAULT_OUTPUT_DIR = Path("./cards")
LOGO_PATH = Path(__file__).parent / "logo.png"


# ═════════════════════════════════════════════════════════════════
#  Drawing helpers
# ═════════════════════════════════════════════════════════════════

def draw_qr(c: canvas.Canvas, x: float, y: float, size: float,
            data: str, fill_color=black):
    """Render a QR code onto the canvas."""
    qr = QrCodeWidget(data)
    qr.barWidth = size
    qr.barHeight = size
    qr.barLevel = "M"
    qr.barFillColor = fill_color
    d = Drawing(size, size)
    d.add(qr)
    renderPDF.draw(d, c, x, y)


def draw_header(c: canvas.Canvas, y_cursor: float) -> float:
    """Header with logo image and subtitle."""
    logo_h = 0.42 * inch
    padding_top = 0.18 * inch
    padding_bottom = 0.06 * inch

    # ── Logo ─────────────────────────────────────────────────────
    img = ImageReader(str(LOGO_PATH))
    iw, ih = img.getSize()
    aspect = iw / ih
    logo_w = logo_h * aspect

    logo_y = y_cursor - padding_top - logo_h
    logo_x = (PAGE_W - logo_w) / 2
    c.drawImage(str(LOGO_PATH), logo_x, logo_y, logo_w, logo_h, mask='auto')

    # ── Subtitle ─────────────────────────────────────────────────
    subtitle_y = logo_y - 12
    c.setFillColor(black)
    c.setFont("Ubuntu", 8)
    c.drawCentredString(PAGE_W / 2, subtitle_y, "LoRa Mesh Radio  ·  Community Network")

    return subtitle_y - padding_bottom


def draw_divider(c: canvas.Canvas, y_cursor: float) -> float:
    """Horizontal divider line."""
    y = y_cursor - 10
    c.setStrokeColor(black)
    c.setLineWidth(0.5)
    c.line(MARGIN, y, PAGE_W - MARGIN, y)
    return y


def draw_section_title(c: canvas.Canvas, y: float, title: str) -> float:
    """Small underlined section header."""
    y -= 15
    c.setFont("Ubuntu-Bold", 9.5)
    c.setFillColor(black)
    c.drawString(MARGIN, y, title.upper())
    y -= 2.5
    return y


def draw_wrapped_text(c: canvas.Canvas, x: float, y: float,
                      text: str, font: str, size: float,
                      max_width: float, leading: float) -> float:
    """Draw text with word wrapping. Returns the y after the last line."""
    c.setFont(font, size)
    words = text.split()
    line = ""
    for word in words:
        test = f"{line} {word}".strip()
        if c.stringWidth(test, font, size) > max_width:
            c.drawString(x, y, line)
            y -= leading
            line = word
        else:
            line = test
    if line:
        c.drawString(x, y, line)
        y -= leading
    return y


def draw_hardware_section(c: canvas.Canvas, y_cursor: float) -> float:
    """Hardware blurb (prose, not a spec table)."""
    y = draw_section_title(c, y_cursor, "Hardware")

    blurb = (
        "This radio is loaded with a pre-configured Meshcore firmware and powered by a Heltec T114 and 2Ah battery. "
        "The case is custom made and printed by Austin Mesh members."
    )

    y -= 12
    y = draw_wrapped_text(
        c, MARGIN + 8, y, blurb,
        font="Ubuntu", size=8.5,
        max_width=CONTENT_W - 16, leading=12,
    )
    return y


def backticks_to_markup(text: str) -> str:
    """Convert `backtick` spans to UbuntuMono-Bold <font> tags for Paragraph."""
    return re.sub(r'`([^`]+)`', r'<font face="UbuntuMono-Bold">\1</font>', text)


STEP_STYLE = ParagraphStyle(
    "step", fontName="Ubuntu", fontSize=8.5, leading=11,
)


def draw_connection_section(c: canvas.Canvas, y_cursor: float) -> float:
    """Numbered BLE pairing walkthrough."""
    y = draw_section_title(c, y_cursor, "Quick Connect via BLE")

    steps = [
        "Install the MeshCore app (iOS or Android).",
        "Enable Bluetooth on your phone.",
        "Open MeshCore and tap `Connect` in the upper right.",
        "Select your `ATXMC-####` radio from the list.",
        "Enter the BLE PIN shown on the radio screen.",
        "You're connected! The app will sync automatically.",
    ]

    y -= 13
    num_x = MARGIN + 8
    text_x = MARGIN + 26
    text_w = CONTENT_W - 26
    for i, text in enumerate(steps, 1):
        c.setFont("Ubuntu-Bold", 8.5)
        c.drawString(num_x, y, f"{i}.")
        p = Paragraph(backticks_to_markup(text), STEP_STYLE)
        p.wrapOn(c, text_w, 999)
        p.drawOn(c, text_x, y - p.height + 9)
        y -= max(p.height, 12.5)

    return y


def draw_first_contact_section(c: canvas.Canvas, y_cursor: float) -> float:
    """First Contact instructions."""
    y = draw_section_title(c, y_cursor, "First Contact")

    blurb = (
        'Test your connection to nearby repeaters by sending a message '
        'to the "Public" channel. If your message was heard and repeated '
        'you will see a "1 Repeats" under the message bubble.'
    )

    y -= 12
    y = draw_wrapped_text(
        c, MARGIN + 8, y, blurb,
        font="Ubuntu", size=8.5,
        max_width=CONTENT_W - 16, leading=12,
    )
    return y


def draw_tips_section(c: canvas.Canvas, y_cursor: float) -> float:
    """General tips."""
    y = draw_section_title(c, y_cursor, "Good to Know")

    tips = [
        "Charges can last a couple days to a full week.",
        "Disable GPS for better battery life.",
    ]

    y -= 13
    for tip in tips:
        c.setFont("Ubuntu", 8)
        c.drawString(MARGIN + 8, y, f"•  {tip}")
        y -= 11.5

    return y


def draw_footer(c: canvas.Canvas):
    """Bottom tagline with thin rule."""
    y = MARGIN + 0
    c.setStrokeColor(black)
    c.setLineWidth(0.25)
    c.line(MARGIN, y + 5, PAGE_W - MARGIN, y + 5)
    c.setFont("Ubuntu", 6.5)
    c.setFillColor(black)
    c.drawCentredString(
        PAGE_W / 2, y - 4,
        "austinmesh.org  ·  Community-powered mesh networking for Austin, TX",
    )


# ═════════════════════════════════════════════════════════════════
#  Page composition
# ═════════════════════════════════════════════════════════════════

def render_card_page(c: canvas.Canvas):
    """Render one complete card page onto the canvas (does NOT save)."""
    y = PAGE_H
    y = draw_header(c, y)
    y = draw_divider(c, y)
    y = draw_hardware_section(c, y)
    y = draw_connection_section(c, y)
    y = draw_first_contact_section(c, y)
    y = draw_tips_section(c, y)
    draw_footer(c)


# ═════════════════════════════════════════════════════════════════
#  Public API
# ═════════════════════════════════════════════════════════════════

def generate_card(output_path: str | Path):
    """Generate a single-page static PDF info card."""
    c = canvas.Canvas(str(output_path), pagesize=(PAGE_W, PAGE_H))
    render_card_page(c)
    c.save()


# ═════════════════════════════════════════════════════════════════
#  CLI
# ═════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Generate Austin Mesh radio info card (4×6 thermal label).",
    )
    parser.add_argument(
        "--out", "-o", metavar="FILE",
        help="output PDF path; defaults to ./cards/radio_card.pdf",
    )
    args = parser.parse_args()

    output_path = Path(args.out) if args.out else DEFAULT_OUTPUT_DIR / "radio_card.pdf"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    generate_card(output_path)
    print(f"Card → {output_path}")


if __name__ == "__main__":
    main()

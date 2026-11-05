#!/usr/bin/env python3
"""Writes the dummy files the end-to-end scenarios attach and read: PDFs (one page, three pages, forty pages, a damaged
one), a sketch PNG, a large JPEG and a short WAV tone. Needs ImageMagick (magick) for the images and Ghostscript (gs) for
the forty-page PDF; the rest is plain Python.

  make_fixtures.py OUT_DIR
"""
import math
import os
import struct
import subprocess
import sys
import wave

out = sys.argv[1] if len(sys.argv) > 1 else "fixtures"
os.makedirs(out, exist_ok=True)


def pdf(path, pages):
    """A small valid PDF with Helvetica text lines and a blue rectangle on each page."""
    objs = []

    def add(s):
        objs.append(s)
        return len(objs)

    font = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    contents = []
    for lines in pages:
        stream = "BT /F1 14 Tf 72 760 Td 18 TL\n" + "\n".join(
            "(%s) '" % l.replace("(", "\\(").replace(")", "\\)") for l in lines) + "\nET\n"
        stream += "0.2 0.4 0.8 RG 2 w 72 300 m 300 300 l 300 450 l 72 450 l h S\n"
        contents.append(add("<< /Length %d >>\nstream\n%sendstream" % (len(stream), stream)))
    pages_id = len(objs) + len(pages) + 1
    kids = [add("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources << /Font << /F1 %d 0 R >> >> >>"
                % (pages_id, c, font)) for c in contents]
    add("<< /Type /Pages /Kids [%s] /Count %d >>" % (" ".join("%d 0 R" % k for k in kids), len(kids)))
    catalog = add("<< /Type /Catalog /Pages %d 0 R >>" % pages_id)
    text, offsets = "%PDF-1.4\n", []
    for i, o in enumerate(objs, 1):
        offsets.append(len(text.encode("latin-1")))
        text += "%d 0 obj\n%s\nendobj\n" % (i, o)
    xref = len(text.encode("latin-1"))
    text += "xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1) + "".join("%010d 00000 n \n" % o for o in offsets)
    text += "trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, catalog, xref)
    open(path, "wb").write(text.encode("latin-1"))


pdf(os.path.join(out, "spec-sheet.pdf"), [["BRACKET SPEC SHEET", "Plate: 120 x 80 mm, thickness 6 mm",
                                           "Holes: 4 x D10 at 15 mm from each edge", "Material: S235 steel",
                                           "Title block: OpenCAD test"]])
pdf(os.path.join(out, "drawing-brief.pdf"), [["DRAWING BRIEF - page 1", "Room: 5000 x 4000 mm", "Door: 900 mm on the south wall"],
                                             ["page 2", "Window: 1200 mm centred on the north wall", "Walls: 200 mm thick"],
                                             ["page 3", "Dimension every wall", "Layer names: Walls, Doors, Windows, Dims"]])
with open(os.path.join(out, "spec-sheet.pdf"), "rb") as f:
    open(os.path.join(out, "broken.pdf"), "wb").write(f.read()[:400])  # cut off: a damaged file
subprocess.run(["gs", "-q", "-dNOPAUSE", "-dBATCH", "-sDEVICE=pdfwrite", "-o", os.path.join(out, "many-pages.pdf"), "-c",
                "1 1 40 { /Helvetica findfont 24 scalefont setfont 72 700 moveto (Page ) show 3 string cvs show showpage } for"],
               check=False)
subprocess.run(["magick", "-size", "320x200", "xc:white", "-fill", "none", "-stroke", "#1f6feb", "-strokewidth", "4", "-draw",
                "rectangle 40,40 280,160", "-draw", "circle 160,100 160,130", "-stroke", "black", "-strokewidth", "1",
                "-pointsize", "18", "-draw", "text 50,30 'SKETCH 240x120 hole D60'", os.path.join(out, "sketch.png")], check=False)
subprocess.run(["magick", "-size", "4000x3000", "gradient:#88aaff-#ffffff", os.path.join(out, "big-photo.jpg")], check=False)
w = wave.open(os.path.join(out, "tone.wav"), "wb")  # 1.5 s of 440 Hz, 16 kHz mono 16-bit: what the recorder writes
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(16000)
w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 16000))) for i in range(24000)))
w.close()
print("fixtures in", out)

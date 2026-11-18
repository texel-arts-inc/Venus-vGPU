# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
from bisect import bisect_right
import csv
from html.parser import HTMLParser
from pathlib import Path
import struct

class Rows(HTMLParser):
    def __init__(self):
        super().__init__()
        self.rows = []
        self.row = None
        self.cell = None

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell = []

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None:
            self.row.append("".join(self.cell).strip())
            self.cell = None
        elif tag == "tr" and self.row is not None:
            self.rows.append(self.row)
            self.row = None

def get_reflectance(path):
    parser = Rows()
    parser.feed(path.read_text(encoding="utf-8"))
    header = ["wavelength", "white", "green", "red"]
    start = next((i for i, row in enumerate(parser.rows)
                  if [cell.lower() for cell in row] == header), None)
    if start is None:
        raise ValueError(f"{path}: Cornell surface reflectance table not found")
    table = [tuple(map(float, row)) for row in parser.rows[start + 1:start + 77]]
    if [row[0] for row in table] != list(range(400, 701, 4)):
        raise ValueError(f"{path}: expected reflectance samples from 400 to 700 nm")
    return tuple(zip(*table))

def get_source(path):
    data = path.read_bytes()
    arrays = {}
    offset = 0
    while offset < len(data):
        kind, rows, cols, imaginary, name_length = struct.unpack_from(">5i", data, offset)
        offset += 20
        name = data[offset:offset + name_length].rstrip(b"\0").decode("ascii")
        offset += name_length
        if kind not in (1000, 1001) or imaginary:
            raise ValueError(f"{path}: expected the original big-endian MATLAB v4 data")
        count = rows * cols
        arrays[name] = struct.unpack_from(f">{count}d", data, offset)
        offset += count * 8
    wavelength, watts = arrays["wavelen"], arrays["source"]
    if len(wavelength) != 1024 or len(watts) != 1024 or wavelength[0] > 400 or wavelength[-1] < 700:
        raise ValueError(f"{path}: expected the 1,024-sample Cornell lamp measurement")
    return wavelength, watts

def lerp(xs, ys, x):
    i = min(bisect_right(xs, x) - 1, len(xs) - 2)
    t = (x - xs[i]) / (xs[i + 1] - xs[i])
    return ys[i] * (1 - t) + ys[i + 1] * t

def mul(matrix, vector):
    return tuple(sum(a * b for a, b in zip(row, vector)) for row in matrix)

parser = argparse.ArgumentParser(description="Build the Cornell Box spectral lookup table")
parser.add_argument("cornell_html", type=Path)
parser.add_argument("lamp_mat", type=Path)
parser.add_argument("cie_csv", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()

wavelength, *reflectance = get_reflectance(args.cornell_html)
measured_wavelength, measured_source = get_source(args.lamp_mat)
with args.cie_csv.open(newline="", encoding="ascii") as cmf:
    observer = {int(w): tuple(map(float, xyz)) for w, *xyz in csv.reader(cmf)}

# Preintegrate the measured lamp spectrum times the product of measured
# diffuse reflectances for the ordered ray tracer's bounce counts.
samples = []
for nm in range(400, 701):
    irradiance = lerp(measured_wavelength, measured_source, nm)
    area = 0.5 if nm in (400, 700) else 1.0
    xyz = tuple(irradiance * a * area for a in observer[nm])
    materials = tuple(lerp(wavelength, row, nm) for row in reflectance)
    samples.append((xyz, materials))

def integrate(white, green, red):
    total = [0.0, 0.0, 0.0]
    for xyz, rho in samples:
        bounce = rho[0] ** white * rho[1] ** green * rho[2] ** red
        for i, v in enumerate(xyz):
            total[i] += bounce * v
    return total

lamp = integrate(0, 0, 0)
bradford = ((.8951, .2664, -.1614), (-.7502, 1.7135, .0367), (.0389, -.0685, 1.0296))
inverse = ((.9869929, -.1470543, .1599627), (.4323053, .5183603, .0492912), (-.0085287, .0400428, .9684867))
xyz_to_srgb = ((3.2404542, -1.5371385, -.4985314), (-.9692660, 1.8760108, .0415560), (.0556434, -.2040259, 1.0572252))
source_white = mul(bradford, [v / lamp[1] for v in lamp])
target_white = mul(bradford, (.95047, 1.0, 1.08883))
# The original demo's lamp luminance sets exposure. Bradford adapts the
# measured lamp white to D65 to display the scene with camera white balance.
exposure = .2126 * 20 + .7152 * 18 + .0722 * 15.5
scale = [exposure / lamp[1] * t / s for s, t in zip(source_white, target_white)]

def rgb(xyz):
    adapted = [x * s for x, s in zip(mul(bradford, xyz), scale)]
    return mul(xyz_to_srgb, mul(inverse, adapted))

values = []
for w in range(6):
    for g in range(6):
        for r in range(6):
            color = rgb(integrate(w, g, r)) if w + g + r <= 5 else (0, 0, 0)
            values.append("    vec3(" + ", ".join(f"{a:.7f}" for a in color) + ")")
array = "const vec3 spectralRadiance[216] = vec3[216](\n" + ",\n".join(values) + "\n);"
args.output.write_text(array + "\n", encoding="utf-8")

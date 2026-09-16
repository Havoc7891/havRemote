#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import argparse
import math
import os
import struct
import sys
import traceback
import zlib
from dataclasses import dataclass


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ERROR_LOG_PATH = os.path.join(os.environ.get("TEMP", os.path.dirname(ROOT)), "havRemote-generate-icon-error.txt")


def write_error_log(message):
    with open(ERROR_LOG_PATH, "w", encoding="utf-8") as log:
        log.write(message)


def report_uncaught_exception(exception_type, exception, tb):
    message = "".join(traceback.format_exception(exception_type, exception, tb))
    diagnostic = (
        "Icon generation failed.\n\n"
        f"Python: {sys.executable}\n"
        f"Working directory: {os.getcwd()}\n"
        f"Project root: {ROOT}\n"
        f"{message}"
    )

    try:
        write_error_log(diagnostic)
    except Exception:
        pass

    print(diagnostic, file=sys.stderr)
    print(f"Failure details written to: {ERROR_LOG_PATH}", file=sys.stderr)


sys.excepthook = report_uncaught_exception

DEFAULT_OUTPUT_DIR = os.path.join(ROOT, "resources", "icons")
FONT_PATH = os.path.join(ROOT, "resources", "font", "Oswald.ttf")
SIZES = (16, 20, 24, 32, 48)
# High-resolution PNG for documentation
PNG_SIZE = 512
SUPERSAMPLE = 4
APP_BACKGROUND = (10, 10, 10, 255)
APP_TEXT = (255, 255, 255, 255)
ACCENT = (196, 0, 0, 255)
PLUG_ANGLE_DEGREES = 18
PLUG_FLIP_HORIZONTAL = True
PLUG_Y_OFFSET = -0.75
WORDMARK_Y = 2.5


@dataclass(frozen=True)
class RoundRect:
    x: float
    y: float
    w: float
    h: float
    r: float
    role: str = "glyph"


@dataclass(frozen=True)
class Polyline:
    points: tuple
    width: float
    role: str = "glyph"


@dataclass(frozen=True)
class Path:
    contours: tuple
    role: str = "glyph"


APP_PALETTE = {
    "background": APP_BACKGROUND,
    "text": APP_TEXT,
    "accent": ACCENT,
}


def point_in_poly(x, y, points):
    inside = False
    j = len(points) - 1
    for i in range(len(points)):
        xi, yi = points[i]
        xj, yj = points[j]
        intersects = ((yi > y) != (yj > y)) and (
            x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-9) + xi
        )
        if intersects:
            inside = not inside
        j = i
    return inside


def distance_to_segment(px, py, ax, ay, bx, by):
    dx = bx - ax
    dy = by - ay
    if dx == 0 and dy == 0:
        return math.hypot(px - ax, py - ay)
    t = max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    qx = ax + t * dx
    qy = ay + t * dy
    return math.hypot(px - qx, py - qy)


def cubic_points(p0, p1, p2, p3, steps=10):
    points = []
    for i in range(steps + 1):
        t = i / steps
        mt = 1 - t
        x = (
            mt * mt * mt * p0[0]
            + 3 * mt * mt * t * p1[0]
            + 3 * mt * t * t * p2[0]
            + t * t * t * p3[0]
        )
        y = (
            mt * mt * mt * p0[1]
            + 3 * mt * mt * t * p1[1]
            + 3 * mt * t * t * p2[1]
            + t * t * t * p3[1]
        )
        points.append((x, y))
    return tuple(points)


def quadratic_points(p0, p1, p2, steps=8):
    points = []
    for i in range(1, steps + 1):
        t = i / steps
        mt = 1 - t
        x = mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0]
        y = mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]
        points.append((x, y))
    return tuple(points)


def read_u16(data, offset):
    return struct.unpack_from(">H", data, offset)[0]


def read_i16(data, offset):
    return struct.unpack_from(">h", data, offset)[0]


def read_u32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def read_i8(data, offset):
    return struct.unpack_from(">b", data, offset)[0]


def read_f2dot14(data, offset):
    return read_i16(data, offset) / 16384


def flatten_quadratic_contour(contour):
    if not contour:
        return ()

    points = []
    for index, point in enumerate(contour):
        points.append(point)
        next_point = contour[(index + 1) % len(contour)]
        if not point[2] and not next_point[2]:
            points.append(((point[0] + next_point[0]) / 2, (point[1] + next_point[1]) / 2, True))

    start_index = next((index for index, point in enumerate(points) if point[2]), None)
    if start_index is None:
        return ()

    points = points[start_index:] + points[:start_index] + [points[start_index]]
    output = [(points[0][0], points[0][1])]
    current = output[0]
    index = 1

    while index < len(points):
        point = points[index]
        if point[2]:
            current = (point[0], point[1])
            output.append(current)
            index += 1
        else:
            next_point = points[index + 1]
            target = (next_point[0], next_point[1])
            output.extend(quadratic_points(current, (point[0], point[1]), target))
            current = target
            index += 2

    return tuple(output)


class TrueTypeFont:
    def __init__(self, path):
        with open(path, "rb") as font:
            self.data = font.read()

        table_count = read_u16(self.data, 4)
        self.tables = {}
        for index in range(table_count):
            offset = 12 + index * 16
            tag = self.data[offset:offset + 4].decode("ascii")
            self.tables[tag] = (read_u32(self.data, offset + 8), read_u32(self.data, offset + 12))

        head = self.table("head")
        hhea = self.table("hhea")
        maxp = self.table("maxp")
        self.index_to_loc_format = read_i16(head, 50)
        self.glyph_count = read_u16(maxp, 4)
        self.metric_count = read_u16(hhea, 34)
        self.advance_widths = self.read_advance_widths()
        self.glyph_offsets = self.read_glyph_offsets()
        self.cmap = self.read_cmap()
        self._glyph_cache = {}

    def table(self, tag):
        offset, length = self.tables[tag]
        return self.data[offset:offset + length]

    def read_advance_widths(self):
        hmtx = self.table("hmtx")
        widths = []
        last_width = 0
        offset = 0
        for index in range(self.glyph_count):
            if index < self.metric_count:
                last_width = read_u16(hmtx, offset)
                offset += 4
            else:
                offset += 2
            widths.append(last_width)
        return widths

    def read_glyph_offsets(self):
        loca = self.table("loca")
        offsets = []
        for index in range(self.glyph_count + 1):
            if self.index_to_loc_format == 0:
                offsets.append(read_u16(loca, index * 2) * 2)
            else:
                offsets.append(read_u32(loca, index * 4))
        return offsets

    def read_cmap(self):
        cmap = self.table("cmap")
        subtables = []
        for index in range(read_u16(cmap, 2)):
            entry = 4 + index * 8
            platform = read_u16(cmap, entry)
            encoding = read_u16(cmap, entry + 2)
            offset = read_u32(cmap, entry + 4)
            glyph_map = self.parse_cmap_subtable(cmap[offset:])
            priority = 0
            if platform == 3 and encoding == 10:
                priority = 3
            elif platform == 3 and encoding in {1, 0}:
                priority = 2
            elif platform == 0:
                priority = 1
            subtables.append((priority, glyph_map))

        if not subtables:
            return {}

        return max(subtables, key=lambda item: item[0])[1]

    def parse_cmap_subtable(self, subtable):
        table_format = read_u16(subtable, 0)
        if table_format == 4:
            return self.parse_cmap_format4(subtable)
        if table_format == 12:
            return self.parse_cmap_format12(subtable)
        return {}

    def parse_cmap_format4(self, subtable):
        glyph_map = {}
        segment_count = read_u16(subtable, 6) // 2
        end_offset = 14
        start_offset = end_offset + segment_count * 2 + 2
        delta_offset = start_offset + segment_count * 2
        range_offset = delta_offset + segment_count * 2

        for index in range(segment_count):
            end_code = read_u16(subtable, end_offset + index * 2)
            start_code = read_u16(subtable, start_offset + index * 2)
            delta = read_i16(subtable, delta_offset + index * 2)
            range_value = read_u16(subtable, range_offset + index * 2)

            if start_code == 0xFFFF and end_code == 0xFFFF:
                continue

            for codepoint in range(start_code, end_code + 1):
                if range_value == 0:
                    glyph_map[codepoint] = (codepoint + delta) & 0xFFFF
                else:
                    glyph_offset = range_offset + index * 2 + range_value + (codepoint - start_code) * 2
                    glyph_id = read_u16(subtable, glyph_offset)
                    if glyph_id:
                        glyph_id = (glyph_id + delta) & 0xFFFF
                    glyph_map[codepoint] = glyph_id
        return glyph_map

    def parse_cmap_format12(self, subtable):
        glyph_map = {}
        group_count = read_u32(subtable, 12)
        for index in range(group_count):
            offset = 16 + index * 12
            start_code = read_u32(subtable, offset)
            end_code = read_u32(subtable, offset + 4)
            start_glyph = read_u32(subtable, offset + 8)
            for codepoint in range(start_code, end_code + 1):
                glyph_map[codepoint] = start_glyph + codepoint - start_code
        return glyph_map

    def glyph_index(self, character):
        return self.cmap.get(ord(character), 0)

    def advance_width(self, glyph_index):
        if glyph_index < len(self.advance_widths):
            return self.advance_widths[glyph_index]
        return self.advance_widths[0]

    def glyph_contours(self, glyph_index, depth=0):
        if glyph_index in self._glyph_cache:
            return self._glyph_cache[glyph_index]
        if depth > 8:
            return ()

        glyf = self.table("glyf")
        start = self.glyph_offsets[glyph_index]
        end = self.glyph_offsets[glyph_index + 1]
        if start == end:
            self._glyph_cache[glyph_index] = ()
            return ()

        offset = start
        contour_count = read_i16(glyf, offset)
        if contour_count >= 0:
            contours = self.read_simple_glyph(glyf, offset, contour_count)
        else:
            contours = self.read_compound_glyph(glyf, offset, depth)

        self._glyph_cache[glyph_index] = contours
        return contours

    def read_simple_glyph(self, glyf, offset, contour_count):
        point_offset = offset + 10
        end_points = [read_u16(glyf, point_offset + index * 2) for index in range(contour_count)]
        point_offset += contour_count * 2
        instruction_length = read_u16(glyf, point_offset)
        point_offset += 2 + instruction_length
        point_count = end_points[-1] + 1 if end_points else 0

        flags = []
        while len(flags) < point_count:
            flag = glyf[point_offset]
            point_offset += 1
            flags.append(flag)
            if flag & 0x08:
                repeat_count = glyf[point_offset]
                point_offset += 1
                flags.extend([flag] * repeat_count)

        xs = []
        x = 0
        for flag in flags:
            if flag & 0x02:
                delta = glyf[point_offset]
                point_offset += 1
                x += delta if flag & 0x10 else -delta
            elif not flag & 0x10:
                x += read_i16(glyf, point_offset)
                point_offset += 2
            xs.append(x)

        ys = []
        y = 0
        for flag in flags:
            if flag & 0x04:
                delta = glyf[point_offset]
                point_offset += 1
                y += delta if flag & 0x20 else -delta
            elif not flag & 0x20:
                y += read_i16(glyf, point_offset)
                point_offset += 2
            ys.append(y)

        contours = []
        start = 0
        for end in end_points:
            contours.append(tuple((xs[index], ys[index], bool(flags[index] & 0x01)) for index in range(start, end + 1)))
            start = end + 1
        return tuple(contours)

    def read_compound_glyph(self, glyf, offset, depth):
        contours = []
        point_offset = offset + 10
        while True:
            flags = read_u16(glyf, point_offset)
            glyph_index = read_u16(glyf, point_offset + 2)
            point_offset += 4

            if flags & 0x0001:
                arg1 = read_i16(glyf, point_offset)
                arg2 = read_i16(glyf, point_offset + 2)
                point_offset += 4
            else:
                arg1 = read_i8(glyf, point_offset)
                arg2 = read_i8(glyf, point_offset + 1)
                point_offset += 2

            dx = arg1 if flags & 0x0002 else 0
            dy = arg2 if flags & 0x0002 else 0
            xx = yy = 1
            xy = yx = 0

            if flags & 0x0008:
                xx = yy = read_f2dot14(glyf, point_offset)
                point_offset += 2
            elif flags & 0x0040:
                xx = read_f2dot14(glyf, point_offset)
                yy = read_f2dot14(glyf, point_offset + 2)
                point_offset += 4
            elif flags & 0x0080:
                xx = read_f2dot14(glyf, point_offset)
                xy = read_f2dot14(glyf, point_offset + 2)
                yx = read_f2dot14(glyf, point_offset + 4)
                yy = read_f2dot14(glyf, point_offset + 6)
                point_offset += 8

            for contour in self.glyph_contours(glyph_index, depth + 1):
                contours.append(tuple(
                    (point[0] * xx + point[1] * xy + dx, point[0] * yx + point[1] * yy + dy, point[2])
                    for point in contour
                ))

            if not flags & 0x0020:
                break

        if flags & 0x0100:
            instruction_length = read_u16(glyf, point_offset)
            point_offset += 2 + instruction_length

        return tuple(contours)


_FONT_CACHE = {}


def load_font(path):
    if path not in _FONT_CACHE:
        _FONT_CACHE[path] = TrueTypeFont(path)
    return _FONT_CACHE[path]


def text_path(text, x, y, width, height, role):
    font = load_font(FONT_PATH)
    raw_contours = []
    cursor = 0

    for character in text:
        glyph_index = font.glyph_index(character)
        for contour in font.glyph_contours(glyph_index):
            raw_contours.append(tuple((cursor + point[0], point[1], point[2]) for point in contour))
        cursor += font.advance_width(glyph_index)

    flattened = tuple(flatten_quadratic_contour(contour) for contour in raw_contours)
    flattened = tuple(contour for contour in flattened if contour)
    if not flattened:
        return Path((), role)

    xs = [point[0] for contour in flattened for point in contour]
    ys = [point[1] for contour in flattened for point in contour]
    left, right = min(xs), max(xs)
    bottom, top = min(ys), max(ys)
    scale = min(width / (right - left), height / (top - bottom))
    used_width = (right - left) * scale
    used_height = (top - bottom) * scale
    offset_x = x + (width - used_width) / 2 - left * scale
    offset_y = y + (height - used_height) / 2 + top * scale

    return Path(tuple(
        tuple((offset_x + point[0] * scale, offset_y - point[1] * scale) for point in contour)
        for contour in flattened
    ), role)


def contains(shape, x, y):
    if isinstance(shape, RoundRect):
        if not (shape.x <= x <= shape.x + shape.w and shape.y <= y <= shape.y + shape.h):
            return False
        left = shape.x + shape.r
        right = shape.x + shape.w - shape.r
        top = shape.y + shape.r
        bottom = shape.y + shape.h - shape.r
        if left <= x <= right or top <= y <= bottom:
            return True
        cx = left if x < left else right
        cy = top if y < top else bottom
        return math.hypot(x - cx, y - cy) <= shape.r
    if isinstance(shape, Polyline):
        radius = shape.width / 2
        for a, b in zip(shape.points, shape.points[1:]):
            if distance_to_segment(x, y, a[0], a[1], b[0], b[1]) <= radius:
                return True
        return any(math.hypot(x - px, y - py) <= radius for px, py in shape.points)
    if isinstance(shape, Path):
        inside = False
        for contour in shape.contours:
            if point_in_poly(x, y, contour):
                inside = not inside
        return inside
    return False


def shape_bounds(shape):
    if isinstance(shape, RoundRect):
        return shape.x, shape.y, shape.x + shape.w, shape.y + shape.h
    if isinstance(shape, Polyline):
        radius = shape.width / 2
        xs = [point[0] for point in shape.points]
        ys = [point[1] for point in shape.points]
        return min(xs) - radius, min(ys) - radius, max(xs) + radius, max(ys) + radius
    if isinstance(shape, Path):
        xs = [point[0] for contour in shape.contours for point in contour]
        ys = [point[1] for contour in shape.contours for point in contour]
        return min(xs), min(ys), max(xs), max(ys)
    return 0, 0, 24, 24


def transform_shape(shape, scale, dx, dy):
    def point(value):
        return (value[0] * scale + dx, value[1] * scale + dy)

    if isinstance(shape, RoundRect):
        return RoundRect(
            shape.x * scale + dx,
            shape.y * scale + dy,
            shape.w * scale,
            shape.h * scale,
            shape.r * scale,
            shape.role,
        )
    if isinstance(shape, Polyline):
        return Polyline(tuple(point(value) for value in shape.points), shape.width * scale, shape.role)
    if isinstance(shape, Path):
        return Path(tuple(tuple(point(value) for value in contour) for contour in shape.contours), shape.role)
    return shape


def fit_shapes(shapes, x, y, width, height):
    bounds = [shape_bounds(shape) for shape in shapes]
    left = min(bound[0] for bound in bounds)
    top = min(bound[1] for bound in bounds)
    right = max(bound[2] for bound in bounds)
    bottom = max(bound[3] for bound in bounds)
    scale = min(width / (right - left), height / (bottom - top))
    used_width = (right - left) * scale
    used_height = (bottom - top) * scale
    dx = x + (width - used_width) / 2 - left * scale
    dy = y + (height - used_height) / 2 - top * scale
    return [transform_shape(shape, scale, dx, dy) for shape in shapes]


def render(size, shapes, palette):
    high = size * SUPERSAMPLE
    high_pixels = [[(0, 0, 0, 0) for _ in range(high)] for _ in range(high)]
    scale = 24 / high
    shape_infos = [(shape, *shape_bounds(shape)) for shape in shapes]

    for y in range(high):
        cy = (y + 0.5) * scale
        for x in range(high):
            cx = (x + 0.5) * scale
            color = None
            for shape, left, top, right, bottom in shape_infos:
                if cx < left or cx > right or cy < top or cy > bottom:
                    continue
                if contains(shape, cx, cy):
                    color = palette[shape.role]
            if color is not None:
                high_pixels[y][x] = color

    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            total = [0, 0, 0, 0]
            for yy in range(SUPERSAMPLE):
                for xx in range(SUPERSAMPLE):
                    r, g, b, a = high_pixels[y * SUPERSAMPLE + yy][x * SUPERSAMPLE + xx]
                    total[0] += r
                    total[1] += g
                    total[2] += b
                    total[3] += a
            count = SUPERSAMPLE * SUPERSAMPLE
            row.append(tuple(v // count for v in total))
        pixels.append(row)
    return pixels


def dib_from_rgba(pixels):
    size = len(pixels)
    xor = bytearray()
    for row in reversed(pixels):
        for r, g, b, a in row:
            xor += bytes((b, g, r, a))

    mask_stride = ((size + 31) // 32) * 4
    and_mask = bytes(mask_stride * size)
    header = struct.pack(
        "<IiiHHIIiiII",
        40,
        size,
        size * 2,
        1,
        32,
        0,
        len(xor) + len(and_mask),
        0,
        0,
        0,
        0,
    )
    return header + xor + and_mask


def png_chunk(chunk_type, data):
    return (
        struct.pack(">I", len(data))
        + chunk_type
        + data
        + struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF)
    )


def write_png(path, pixels):
    height = len(pixels)
    width = len(pixels[0]) if height else 0
    raw = bytearray()

    for row in pixels:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(png_chunk(b"IEND", b""))


def app_plug(role="accent", size=48):
    def rounded_contour(x, y, width, height, radius, steps=3):
        points = []
        corners = (
            (x + width - radius, y + radius, -90),
            (x + width - radius, y + height - radius, 0),
            (x + radius, y + height - radius, 90),
            (x + radius, y + radius, 180),
        )
        for center_x, center_y, start_angle in corners:
            for step in range(steps + 1):
                angle = math.radians(start_angle + 90 * step / steps)
                points.append((
                    center_x + math.cos(angle) * radius,
                    center_y + math.sin(angle) * radius,
                ))
        return tuple(points)

    body = (
        (8.85, 15.75),
        (15.15, 15.75),
        (15.15, 16.25),
        *cubic_points((15.15, 16.25), (15.10, 17.25),
                      (13.85, 18.20), (13.40, 19.30), steps=8)[1:],
        (10.60, 19.30),
        *cubic_points((10.60, 19.30), (10.15, 18.20),
                      (8.90, 17.25), (8.85, 16.25), steps=8)[1:],
    )

    def angle_plug(shapes, scale_factor):
        # Scale and rotate about the plug's visual center, then reflect its x axis
        angle = math.radians(PLUG_ANGLE_DEGREES)
        cosine = math.cos(angle)
        sine = math.sin(angle)
        horizontal_direction = -1 if PLUG_FLIP_HORIZONTAL else 1
        center_x, center_y = 12.0, 16.70
        offset_x, offset_y = 0.0, PLUG_Y_OFFSET

        def transform_point(point):
            x = (point[0] - center_x) * scale_factor
            y = (point[1] - center_y) * scale_factor
            rotated_x = x * cosine - y * sine
            return (
                center_x + horizontal_direction * rotated_x + offset_x,
                center_y + x * sine + y * cosine + offset_y,
            )

        transformed = []
        for shape in shapes:
            if isinstance(shape, Polyline):
                transformed.append(Polyline(
                    tuple(transform_point(point) for point in shape.points),
                    shape.width * scale_factor,
                    shape.role,
                ))
            elif isinstance(shape, Path):
                transformed.append(Path(
                    tuple(
                        tuple(transform_point(point) for point in contour)
                        for contour in shape.contours
                    ),
                    shape.role,
                ))
        return transformed

    if size <= 20:
        # Wider prongs remain separate when small frames are downsampled
        small_body = (
            (8.10, 15.80),
            (15.90, 15.80),
            (15.90, 16.45),
            *cubic_points((15.90, 16.45), (15.65, 17.70),
                          (14.10, 18.55), (13.35, 19.40), steps=6)[1:],
            (10.65, 19.40),
            *cubic_points((10.65, 19.40), (9.90, 18.55),
                          (8.35, 17.70), (8.10, 16.45), steps=6)[1:],
        )
        small_plug = [
            Path((rounded_contour(8.90, 11.35, 1.60, 3.85, 0.34, steps=2),), role),
            Path((rounded_contour(13.50, 11.35, 1.60, 3.85, 0.34, steps=2),), role),
            Path((rounded_contour(7.40, 14.40, 9.20, 1.62, 0.42, steps=2),), role),
            Path((small_body,), role),
            Path((rounded_contour(10.05, 19.10, 3.90, 1.02, 0.30, steps=2),), role),
            Path((rounded_contour(10.40, 20.02, 3.20, 0.90, 0.28, steps=2),), role),
            Polyline(cubic_points((12.00, 21.18), (12.00, 21.52),
                                  (12.55, 21.78), (13.30, 21.72), steps=6),
                     1.12, role),
        ]
        return angle_plug(small_plug, 0.96)

    # Full-size silhouette: rounded parallel prongs, a broad collar, curved
    # bell housing, two separated relief bands, and a short J-shaped cable.
    plug = [
        Path((rounded_contour(9.35, 11.20, 1.35, 3.80, 0.42),), role),
        Path((rounded_contour(13.30, 11.20, 1.35, 3.80, 0.42),), role),
        Path((rounded_contour(8.25, 14.20, 7.50, 1.30, 0.40),), role),
        Path((body,), role),
        Path((rounded_contour(10.55, 19.58, 2.90, 0.70, 0.28),), role),
        Path((rounded_contour(10.65, 20.48, 2.70, 0.65, 0.26),), role),
        Polyline(cubic_points((12.00, 21.28), (12.02, 21.62),
                              (12.85, 21.95), (13.85, 21.82), steps=10),
                 0.95, role),
    ]

    return angle_plug(plug, 0.94)


def app_icon_shapes(size=48):
    return [
        RoundRect(1.5, 1.5, 21.0, 21.0, 3.6, "background"),
        text_path("hav", 6.6, WORDMARK_Y, 11.0, 7.0, "text"),
        *app_plug("accent", size),
    ]


def WhiteLogoShapes():
    return fit_shapes(app_icon_shapes(PNG_SIZE), 1, 1, 22, 22)


def BadgeLogoShapes():
    # Center the full-detail plug within the tile
    return [
        WhiteLogoShapes()[0],
        *fit_shapes(app_plug("accent", PNG_SIZE), 3, 3, 18, 18),
    ]


def SvgNumber(value):
    number = f"{value:.3f}".rstrip("0").rstrip(".")
    return "0" if number == "-0" else number


def SimplifySvgPoints(points, tolerance=0.003):
    # Bound the outline deviation while keeping embedded badge URLs compact
    if len(points) <= 2:
        return points
    distance, index = max(
        (distance_to_segment(*point, *points[0], *points[-1]), index)
        for index, point in enumerate(points[1:-1], 1)
    )
    if distance <= tolerance:
        return (points[0], points[-1])
    return (
        SimplifySvgPoints(points[:index + 1], tolerance)[:-1]
        + SimplifySvgPoints(points[index:], tolerance)
    )


def SvgContour(points, closed=True):
    points = SimplifySvgPoints(points + points[:1] if closed else points)
    if closed:
        points = points[:-1]
    coordinates = " ".join(f"{SvgNumber(x)} {SvgNumber(y)}" for x, y in points)
    return f"M{coordinates}" + ("Z" if closed else "")


def CutoutLogoSvg(shapes, size=24):
    tile, *cutouts = shapes
    if not isinstance(tile, RoundRect):
        raise TypeError("The white-logo background must be a rounded tile")
    elements = []
    for shape in cutouts:
        if isinstance(shape, Path):
            data = "".join(SvgContour(contour) for contour in shape.contours)
            # The counters remain part of the tile, not the letter cutouts
            elements.append(f'<path fill-rule="evenodd" d="{data}"/>')
        elif isinstance(shape, Polyline):
            data = SvgContour(shape.points, closed=False)
            elements.append(
                f'<path fill="none" stroke="#000" stroke-width="{SvgNumber(shape.width)}" '
                f'stroke-linecap="round" stroke-linejoin="round" d="{data}"/>'
            )
        else:
            raise TypeError(f"Unsupported white-logo shape: {type(shape).__name__}")

    # A luminance mask makes overlapping plug parts a single transparent cutout.
    # Black is used only in the mask, never painted into the visible logo.
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
        'viewBox="0 0 24 24" fill="#fff">\n'
        '<defs><mask id="cutouts" maskUnits="userSpaceOnUse" '
        'maskContentUnits="userSpaceOnUse" x="0" y="0" width="24" height="24">\n'
        '<rect width="24" height="24" fill="#fff"/>\n'
        '<g fill="#000">\n'
        + "\n".join(elements)
        + '\n</g></mask></defs>\n'
        + f'<rect x="{SvgNumber(tile.x)}" y="{SvgNumber(tile.y)}" '
        f'width="{SvgNumber(tile.w)}" height="{SvgNumber(tile.h)}" '
        f'rx="{SvgNumber(tile.r)}" mask="url(#cutouts)"/>\n'
        '</svg>\n'
    )


def WhiteLogoSvg():
    return CutoutLogoSvg(WhiteLogoShapes())


def BadgeLogoSvg():
    return CutoutLogoSvg(BadgeLogoShapes(), size=14)


def WriteSvgLogos(output_dir):
    for name, content in (
        ("havRemoteWhite.svg", WhiteLogoSvg()),
        ("havRemoteBadge.svg", BadgeLogoSvg()),
    ):
        path = os.path.join(output_dir, name)
        with open(path, "w", encoding="utf-8", newline="\n") as svg_file:
            svg_file.write(content)


def write_app_icon(output_dir):
    images = [dib_from_rgba(render(size, app_icon_shapes(size), APP_PALETTE)) for size in SIZES]
    offset = 6 + 16 * len(images)
    entries = bytearray()
    for size, image in zip(SIZES, images):
        entries += struct.pack(
            "<BBBBHHII",
            size,
            size,
            0,
            0,
            1,
            32,
            len(image),
            offset,
        )
        offset += len(image)

    with open(os.path.join(output_dir, "havRemote.ico"), "wb") as icon_file:
        icon_file.write(struct.pack("<HHH", 0, 1, len(images)))
        icon_file.write(entries)
        for image in images:
            icon_file.write(image)

    pixels = render(PNG_SIZE, app_icon_shapes(PNG_SIZE), APP_PALETTE)
    write_png(os.path.join(output_dir, "havRemote.png"), pixels)


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Regenerate havRemote's procedural Windows application icon, "
            "offline-help favicon, documentation logo, and white full-logo and badge SVGs."
        )
    )
    parser.add_argument(
        "--output-dir",
        default=DEFAULT_OUTPUT_DIR,
        help=(
            "Destination for havRemote.ico (Windows app and offline-help favicon), "
            "havRemote.png (documentation logo), havRemoteWhite.svg (full white logo), "
            "and havRemoteBadge.svg (plug-only white badge logo). "
            "Relative paths are resolved from the project root."
        ),
    )
    parser.add_argument(
        "--svg-only",
        action="store_true",
        help="Generate only the two white cutout SVGs, leaving the ICO and PNG unchanged.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    output_dir = args.output_dir
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(ROOT, output_dir)
    output_dir = os.path.abspath(output_dir)

    if not os.path.isfile(FONT_PATH):
        raise FileNotFoundError(f"Icon source font not found: {FONT_PATH}")

    os.makedirs(output_dir, exist_ok=True)
    WriteSvgLogos(output_dir)
    if args.svg_only:
        print(f"Generated havRemoteWhite.svg and havRemoteBadge.svg in {output_dir}", flush=True)
    else:
        write_app_icon(output_dir)
        print(
            f"Generated havRemote.ico, havRemote.png, havRemoteWhite.svg, "
            f"and havRemoteBadge.svg in {output_dir}",
            flush=True,
        )


if __name__ == "__main__":
    main()

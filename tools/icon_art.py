"""Original air/OS vector drawing helpers, MIT licence.

A 64-unit canvas and two-unit silhouette outline target Tracker's 32px view.
Only local vector geometry is needed; optional host previews use Pillow.
"""
from pathlib import Path
import argparse
import hvif

INK = '151719'


class Drawing:
    def __init__(self):
        self.icon = {'styles': [], 'paths': [], 'shapes': []}

    def solid(self, color, alpha=255):
        return {'color': hvif.hex_color(color, alpha)}

    def gradient(self, light, dark, start=(10, 6), end=(48, 56)):
        return hvif.linear_gradient(start, end, [(0, hvif.hex_color(light)), (1, hvif.hex_color(dark))])

    def shape(self, points, paint, outline=None, width=2, closed=True, lod=None):
        icon = self.icon
        path = len(icon['paths'])
        icon['paths'].append({'closed': closed, 'points': points})
        for style, stroke in ((paint, False), (outline, True)):
            if style is None:
                continue
            if isinstance(style, str):
                style = self.solid(style)
            if style not in icon['styles']:
                icon['styles'].append(style)
            shape = {'style': icon['styles'].index(style), 'paths': [path]}
            if stroke:
                shape['transformers'] = [{'type': 'stroke', 'width': width, 'join': 2, 'cap': 2, 'miter': 4}]
            if lod:
                shape['lod'] = lod
            icon['shapes'].append(shape)

    def line(self, points, color, width=2, lod=None):
        self.shape(points, None, color, width, closed=False, lod=lod)

    def ellipse(self, x, y, rx, ry, paint, outline=None, width=2, project=None):
        k = 0.55228475
        points = [
            ((x+rx,y),(x+rx,y-k*ry),(x+rx,y+k*ry)),
            ((x,y+ry),(x+k*rx,y+ry),(x-k*rx,y+ry)),
            ((x-rx,y),(x-rx,y+k*ry),(x-rx,y-k*ry)),
            ((x,y-ry),(x-k*rx,y-ry),(x+k*rx,y-ry)),
        ]
        if project:
            points = [tuple(project(p) for p in point) for point in points]
        self.shape(points, paint, outline, width)

    def rounded_rect(self, x, y, w, h, r, paint, outline=None, width=2, project=None):
        k = 0.55228475 * r
        points = [
            ((x+r,y),(x+r-k,y),(x+r,y)),
            ((x+w-r,y),(x+w-r,y),(x+w-r+k,y)),
            ((x+w,y+r),(x+w,y+r-k),(x+w,y+r)),
            ((x+w,y+h-r),(x+w,y+h-r),(x+w,y+h-r+k)),
            ((x+w-r,y+h),(x+w-r+k,y+h),(x+w-r,y+h)),
            ((x+r,y+h),(x+r,y+h),(x+r-k,y+h)),
            ((x,y+h-r),(x,y+h-r+k),(x,y+h-r)),
            ((x,y+r),(x,y+r),(x,y+r-k)),
        ]
        if project:
            points = [tuple(project(p) for p in point) for point in points]
        self.shape(points, paint, outline, width)

    def shadow(self, points):
        self.shape(points, hvif.linear_gradient((20, 52), (62, 52), [
            (0, (0, 0, 0, 65)), (0.75, (0, 0, 0, 40)), (1, (0, 0, 0, 0))]))

    def save(self, script, relative):
        default = Path(script).resolve().parents[1] / relative
        parser = argparse.ArgumentParser(description='Generate the native HVIF and matching SVG icon.')
        parser.add_argument('output', nargs='?', default=str(default), help='output HVIF path')
        parser.add_argument('preview', nargs='?', help='optional 256px host PNG preview (requires Pillow)')
        args = parser.parse_args()
        out = Path(args.output)
        preview = Path(args.preview) if args.preview else None
        if preview and preview.resolve() in (out.resolve(), out.with_suffix('.svg').resolve()):
            parser.error('the preview must not overwrite the HVIF or SVG')
        out.parent.mkdir(parents=True, exist_ok=True)
        data = hvif.encode(self.icon)
        decoded = hvif.decode(data)
        if hvif.encode(decoded) != data:
            raise ValueError('HVIF roundtrip changed the drawing')
        out.write_bytes(data)
        out.with_suffix('.svg').write_text(hvif.to_svg(decoded), encoding='utf-8')
        if preview:
            hvif.preview(decoded, 256).save(preview)
        print(f'{out.name}: {len(data)} bytes, {len(decoded["shapes"])} shapes')


def plane(x, y, slope=0.5):
    """Front face: upright verticals and 2:1 floor-contact edges by default."""
    return lambda points: [(x+u, y+slope*u+v) for u, v in points]

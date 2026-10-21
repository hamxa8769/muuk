"""Draws muuk.ico (the app's icon): a rounded violet-to-blue square with a
white "M" made of two rising strokes. Run once: python make_icon.py"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024  # drawn large, scaled down per size


def draw():
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    # Background: a vertical gradient, then masked to a rounded square.
    grad = Image.new('RGBA', (S, S))
    top, bottom = (124, 77, 255), (41, 121, 255)
    px = grad.load()
    for y in range(S):
        t = y / (S - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(S):
            px[x, y] = c
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((32, 32, S - 32, S - 32), radius=220, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # The "M": two peaks, like a price swinging up twice.
    w = 104
    pts = [(250, 760), (250, 300), (512, 600), (774, 300), (774, 760)]
    d.line(pts, fill=(255, 255, 255, 255), width=w, joint='curve')
    for x, y in (pts[0], pts[-1]):
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=(255, 255, 255, 255))
    return img


if __name__ == '__main__':
    big = draw()
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    big.save(os.path.join(HERE, 'muuk.ico'), sizes=sizes)
    big.resize((256, 256), Image.LANCZOS).save(os.path.join(HERE, 'muuk.png'))
    print('muuk.ico written')

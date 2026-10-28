# Rebuilds moose.gif from moose.webp (the Mini App's running deer: Regalit/shadowlos-web-app,
# develop, public/moose.webp; 512x512, 33 frames, 2.31 s a loop).
#
# Our static Qt has the qgif image plugin but no qwebp (qtimageformats is not built), so the
# app ships the same frames as a GIF. GIF transparency is 1-bit, so each frame is laid on the
# status card's colour (#1A2023) instead; the card is that colour in every view. 70 ms a frame.
# The app mirrors it when painting (the Mini App uses -scale-x-100), so it runs left to right.
#
# Usage: python webp_to_gif.py <moose.webp> <moose.gif> [side=320]   (needs Pillow)
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
side = int(sys.argv[3]) if len(sys.argv) > 3 else 320
im = Image.open(src)
frames = []
for i in range(im.n_frames):
    im.seek(i)
    rgba = im.convert('RGBA').resize((side, side), Image.LANCZOS)
    bg = Image.new('RGBA', rgba.size, (0x1A, 0x20, 0x23, 255))
    bg.alpha_composite(rgba)
    frames.append(bg.convert('RGB').quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                                             dither=Image.Dither.FLOYDSTEINBERG))
frames[0].save(dst, save_all=True, append_images=frames[1:], duration=70, loop=0, optimize=False, disposal=1)
print(len(frames), 'frames')

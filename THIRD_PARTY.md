# Third-party software

These are bundled in `third_party/` and installed into the game folder by Setup.

## cnc-ddraw 7.1.0.0

`third_party/cnc-ddraw/ddraw.dll` is from https://github.com/FunkyFr3sh/cnc-ddraw. It
re-implements DirectDraw so the game's picture can be scaled to any screen. Its settings
for this mod are in `config/ddraw.ini`.

```
MIT License

Copyright (c) 2025 github.com/FunkyFr3sh

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Catmull-Rom shader

`third_party/cnc-ddraw/Shaders/interpolation/catmull-rom-bilinear.glsl` comes with
cnc-ddraw's shader pack (from https://github.com/libretro/glsl-shaders), based on code by
TheRealMJP (https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae). It is
under the MIT License, as its header says.

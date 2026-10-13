# Retro filters: attribution

The menu's retro filters are shaders from the libretro shader collection,
https://github.com/libretro/glsl-shaders, converted unchanged into compute shaders by
scripts/libretro_shaders/convert.py (the wrapper maps libretro's uniforms and texture to a
compute dispatch). Each .comp file keeps the original header with its copyright and license.
Thanks to every author below.

| Menu name | Source | Author | License |
|---|---|---|---|
| CRT: Lottes | [crt/shaders/crt-lottes.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-lottes.glsl) | Timothy Lottes | Public domain |
| CRT: Lottes mini | [crt/shaders/crt-lottes-mini.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-lottes-mini.glsl) | DariusG 2024-25 | GPL-2.0-or-later |
| CRT: Hyllian fast | [crt/shaders/hyllian/crt-hyllian-fast.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/hyllian/crt-hyllian-fast.glsl) | metallic77 | MIT |
| CRT: Hyllian 3D | [crt/shaders/hyllian/crt-hyllian-3d.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/hyllian/crt-hyllian-3d.glsl) | 2011-2016 Hyllian - sergiogdb@gmail.com | MIT |
| CRT: Guest SM | [crt/shaders/guest/crt-guest-sm.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/guest/crt-guest-sm.glsl) | 2019 guest(r) - guest.r@gmail.com | GPL-2.0-or-later |
| CRT: GDV mini | [crt/shaders/crt-gdv-mini.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-gdv-mini.glsl) | metallic77 | GPL-2.0-or-later |
| CRT: Caligari | [crt/shaders/crt-caligari.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-caligari.glsl) | 2011 caligari. | GPL-2.0-or-later |
| CRT: Geom mini | [crt/shaders/crt-geom-mini.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-geom-mini.glsl) | DariusG 2024-2026. | MIT |
| CRT: fake Geom | [crt/shaders/fake-CRT-Geom.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/fake-CRT-Geom.glsl) | DariusG 2023-24-25 | GPL-2.0-or-later |
| CRT: Nobody | [crt/shaders/crt-nobody.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-nobody.glsl) | 2011-2022 Hyllian - sergiogdb@gmail.com | MIT |
| CRT: blurPi scanlines | [crt/shaders/crt-blurPiScanlines.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-blurPiScanlines.glsl) | Oriol Ferrer Mesià (armadillu) | MIT |
| CRT: zfast | [crt/shaders/zfast_crt.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/zfast_crt.glsl) | 2017 Greg Hogan (SoltanGris42) | GPL-2.0-or-later |
| CRT: zfast curved | [crt/shaders/zfast_crt_geo.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/zfast_crt_geo.glsl) | 2017 Greg Hogan (SoltanGris42) | GPL-2.0-or-later |
| CRT: zfast composite | [crt/shaders/zfast_crt_composite.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/zfast_crt_composite.glsl) | metallic 77. | GPL-2.0-or-later |
| CRT: zfast curved S-Video | [crt/shaders/zfast_crt_geo_svideo.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/zfast_crt_geo_svideo.glsl) | 2017 Greg Hogan (SoltanGris42) | GPL-2.0-or-later |
| Scanlines: fract | [scanlines/shaders/scanline-fract.glsl](https://github.com/libretro/glsl-shaders/blob/master/scanlines/shaders/scanline-fract.glsl) | hunterk | Public domain |
| Scanlines: sine | [scanlines/shaders/scanlines-sine-abs.glsl](https://github.com/libretro/glsl-shaders/blob/master/scanlines/shaders/scanlines-sine-abs.glsl) | RiskyJumps | Public domain |
| Scanlines: resolution independent | [scanlines/shaders/res-independent-scanlines.glsl](https://github.com/libretro/glsl-shaders/blob/master/scanlines/shaders/res-independent-scanlines.glsl) | RiskyJumps | Public domain |
| Scanlines: broadcast | [scanlines/shaders/broadcast.glsl](https://github.com/libretro/glsl-shaders/blob/master/scanlines/shaders/broadcast.glsl) | the Free | GPL-2.0-or-later |
| Scanlines: simple | [crt/shaders/torridgristle/ScanlineSimple.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/torridgristle/ScanlineSimple.glsl) | see the original header in the .comp file | Public domain |
| Scanlines: interpolated | [crt/shaders/torridgristle/Scanline-Interpolation.glsl](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/torridgristle/Scanline-Interpolation.glsl) | see the original header in the .comp file | Public domain |
| Scanlines: sharp bilinear | [interpolation/shaders/sharp-bilinear-scanlines.glsl](https://github.com/libretro/glsl-shaders/blob/master/interpolation/shaders/sharp-bilinear-scanlines.glsl) | rsn8887 (based on TheMaister) | Public domain |
| Handheld: dot matrix | [handheld/shaders/dot.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/dot.glsl) | Themaister | Public domain |
| Handheld: LCD 3x | [handheld/shaders/lcd3x.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/lcd3x.glsl) | Gigaherz | Public domain |
| Handheld: LCD 1x | [handheld/shaders/lcd1x.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/lcd1x.glsl) | Gigaherz | Public domain |
| Handheld: SameBoy LCD | [handheld/shaders/sameboy-lcd.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/sameboy-lcd.glsl) | LIJI32 | MIT |
| Handheld: zfast LCD | [handheld/shaders/zfast_lcd.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/zfast_lcd.glsl) | 2017 Greg Hogan (SoltanGris42) | GPL-2.0-or-later |
| Handheld: retro v2 | [handheld/shaders/retro-v2.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/retro-v2.glsl) | Hyllian and DOLLS! | GPL-2.0-or-later |
| Handheld: bevel | [handheld/shaders/bevel.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/bevel.glsl) | 2011-2014 Hyllian/Jararaca - sergiogdb@gmail.com | GPL-2.0-or-later |
| Handheld colors: GBA | [handheld/shaders/color/gba-color.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/color/gba-color.glsl) | hunterk | Public domain |
| Handheld colors: GBC | [handheld/shaders/color/gbc-color.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/color/gbc-color.glsl) | hunterk | Public domain |
| Handheld colors: NDS | [handheld/shaders/color/nds-color.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/color/nds-color.glsl) | hunterk | Public domain |
| Handheld colors: PSP | [handheld/shaders/color/psp-color.glsl](https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/color/psp-color.glsl) | hunterk | Public domain |
| Pixels: sharp bilinear | [interpolation/shaders/sharp-bilinear.glsl](https://github.com/libretro/glsl-shaders/blob/master/interpolation/shaders/sharp-bilinear.glsl) | Themaister | Public domain |
| Pixels: sharp bilinear simple | [interpolation/shaders/sharp-bilinear-simple.glsl](https://github.com/libretro/glsl-shaders/blob/master/interpolation/shaders/sharp-bilinear-simple.glsl) | rsn8887 (based on TheMaister) | Public domain |
| Pixels: pixel art AA | [interpolation/shaders/pixel_art_AA.glsl](https://github.com/libretro/glsl-shaders/blob/master/interpolation/shaders/pixel_art_AA.glsl) | DariusG 2023 | GPL-2.0-or-later |
| Pixels: AANN | [interpolation/shaders/aann.glsl](https://github.com/libretro/glsl-shaders/blob/master/interpolation/shaders/aann.glsl) | jimbo1qaz and wareya | MIT |
| Dither: Bayer 8x8 | [dithering/shaders/bayer-matrix-dithering.glsl](https://github.com/libretro/glsl-shaders/blob/master/dithering/shaders/bayer-matrix-dithering.glsl) | Martins Upitis | Free to use (author statement in the header) |
| Dither: simple | [dithering/shaders/simple_dither.glsl](https://github.com/libretro/glsl-shaders/blob/master/dithering/shaders/simple_dither.glsl) | DariusG 2023 | GPL-2.0-or-later |
| Dither: gendither | [dithering/shaders/gendither.glsl](https://github.com/libretro/glsl-shaders/blob/master/dithering/shaders/gendither.glsl) | 2013-2014 leilei | GPL-2.0-or-later |
| Print: CMYK halftone | [misc/shaders/cmyk-halftone-dot.glsl](https://github.com/libretro/glsl-shaders/blob/master/misc/shaders/cmyk-halftone-dot.glsl) | hunterk | Public domain |
| Color: mangler | [misc/shaders/color-mangler.glsl](https://github.com/libretro/glsl-shaders/blob/master/misc/shaders/color-mangler.glsl) | hunterk | Public domain |
| Color: grade | [misc/shaders/grade-no-LUT.glsl](https://github.com/libretro/glsl-shaders/blob/master/misc/shaders/grade-no-LUT.glsl) | 2020-2023 Dogway (Jose Linares) | GPL-2.0-or-later |
| Video: interlacing | [misc/shaders/interlacing.glsl](https://github.com/libretro/glsl-shaders/blob/master/misc/shaders/interlacing.glsl) | hunterk | Public domain |
| Sharpen: adaptive | [sharpen/shaders/adaptive-sharpen.glsl](https://github.com/libretro/glsl-shaders/blob/master/sharpen/shaders/adaptive-sharpen.glsl) | 2015, bacondither | BSD |
| Sharpen: fast | [sharpen/shaders/fast-sharpen.glsl](https://github.com/libretro/glsl-shaders/blob/master/sharpen/shaders/fast-sharpen.glsl) | 2005 - 2019 guest(r) - guest.r@gmail.com | GPL-2.0-or-later |

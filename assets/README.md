# Buddy boot artwork

`buddy_boot_source.png` is the full generated source. `buddy_boot_240.png` is
the 59-color, 240 x 240 RGB565 reference preview matching the palette embedded
in `../BuddyBootAsset.h`. The converter uses a 64-color quantization target and
merges entries that collapse to the same final RGB565 value.

The asset was created with the built-in ImageGen workflow. The three images
provided with the redesign request were used only as style and mood references;
they were not edit targets, and no character, frame, logo, or layout was copied.

## Final generation brief

```text
Use case: stylized-concept
Asset type: 240x240 embedded handheld boot-screen character artwork
Primary request: Create an original retro anime cyberpunk heroine portrait for
a tiny square handheld screen, inspired only by the general mood of the three
reference images.
Scene/backdrop: deep midnight-navy futuristic city at night with sparse cyan
window lights and a subtle magenta horizon glow.
Subject: one original young adult anime heroine, shoulder-up portrait, short
dark-violet bob haircut with cyan rim-light strands, calm confident expression,
and a simple futuristic jacket; no resemblance to any known character.
Style/medium: authentic hand-crafted 16-bit pixel art, crisp square pixels,
limited palette, strong silhouette, deliberate dithering.
Composition/framing: square; face and shoulders centered in the upper and
middle area; bottom 24 percent dark and quiet for the firmware prompt.
Lighting/mood: dreamy late-night arcade mood; magenta key light; cyan rim light.
Palette: near-black navy, dark indigo, muted lavender, hot magenta, pale pink,
electric cyan, warm off-white.
Constraints: exactly one character; no text, numbers, logos, UI frame,
watermark, weapons, hands, transparent gaps, 3D rendering, or copyrighted
characters; readable after downscaling to 240x240.
```

The text, chrome, animation, and `PRESS ANY KEY` prompt are rendered by the
firmware so they remain sharp and can be changed without regenerating artwork.

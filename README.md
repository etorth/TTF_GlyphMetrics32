# TTF_GlyphMetrics32
sample code to extract bounding box of a UTF8 character

Usage:

```sh
./a.out --font-file ./1.ttf --font-size 32 --utf8-text "Google" --draw-texture-box --draw-bounding-box --draw-baseline --font-style-bold --font-style-italic --disable-kerning --font-script=latn
```

The window shows five rows:

1. cropped glyph bounding boxes stitched by glyph metrics
2. horizontally cropped `TTF_RenderGlyph32_Blended()` glyph textures stitched by glyph metrics
3. one texture rendered by `TTF_RenderUTF8_Blended()`
4. uncropped `TTF_RenderGlyph32_Blended()` glyph textures stitched directly
5. per-character `TTF_RenderUTF8_Blended()` textures stitched directly

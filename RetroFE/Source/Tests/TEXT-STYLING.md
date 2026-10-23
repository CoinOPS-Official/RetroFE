# Text menus and letter spacing

`textType="playlist"` on a menu selects playlist titles as text without
requiring an image or video type. `textType="title"` selects ordinary item
titles. Explicit text menus skip media lookup. Existing media menus and
`textFallback="true"` keep their existing behavior.

Use the existing font attributes on `itemDefaults`. Menu text also supports
`textFormat="uppercase"` and `textFormat="lowercase"` on `itemDefaults` or
the menu itself (the menu takes precedence).

```xml
<menu menuIndex="0" type="custom" textType="playlist"
      orientation="horizontal" scrollTime="0.20">
    <itemDefaults font="fonts/ScoutCond-Bold.otf" loadFontSize="32"
                  fontSize="30" textFormat="uppercase"
                  fontColor="FFFFFF" fontGradient="false" fontOutline="0"
                  letterSpacing="1.8" x="center" y="1056"
                  xOrigin="center" yOrigin="center" layer="17"/>
    <item xOffset="-160" alpha="0.20"/>
    <item xOffset="0" alpha="0.36" selected="true"/>
    <item xOffset="160" alpha="0.20"/>
</menu>
```

`letterSpacing` applies to ordinary text, reloadable text, scrolling text,
and local/global high-score text. Its default is `0`, preserving natural
font shaping. Positive values widen spacing; negative values tighten it.
Values are in layout pixels, independent of font raster size, and scale
with the layout at display time. Spaces participate; there is no extra
gap after the last shaped cluster on a line. Combining marks and ligatures
remain together, and the font's normal kerning is retained.

Width measurement, centering, clipping, wrapping, and scrolling use the same
spacing as drawing. Menu slots may override `letterSpacing`, and it may be
animated with `<animate type="letterSpacing" from="0" to="1.8"/>` inside
a normal animation set. Spacing is per component and does not change a shared
font's settings.

The original playlist9 Photoshop template uses Scout Condensed Bold, 60/1000
em tracking, automatic kerning, no faux bold/italic, no text stroke, and an
identity scale transform. At an effective font size near 30 pixels, its
tracking corresponds to about 1.8 layout pixels. Rasterization and the
original Photoshop anti-aliasing can still cause small visual differences.
On the Pi's Wheel layout, `fontSize="30"` and `letterSpacing="2.2"` give a
closer measured match to the original PNGs' visible height and width.

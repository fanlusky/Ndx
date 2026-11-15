# Ndx logo

- `ndx-logo.svg`: original vector artwork.
- `ndx-logo.png`: transparent 1024 x 1024 image, centered with 3% padding.
- `ndx-logo.ico`: Windows icon with 256, 128, 64, 48, 44, 40, 36, 32, 28, 24, 20 and 16 px frames.
- `ndx_icon.rc`: application icon resource (`NDX_ICON`).

Regenerate from the repository root with ImageMagick on PATH:

```powershell
magick -background none -density 256 resources/icons/ndx-logo.svg -background none -gravity center -extent 1330x1330 -filter Lanczos -resize 1024x1024 resources/icons/ndx-logo.png
.\scripts\convert-png-to-ico.ps1 .\resources\icons\ndx-logo.svg .\resources\icons\ndx-logo.ico -Force
```

The PNG canvas is calculated from the SVG's 468 px height at 256 DPI, with 3% transparent padding on each side. Recalculate the canvas if the SVG dimensions change. The ICO conversion script calculates its canvas automatically.

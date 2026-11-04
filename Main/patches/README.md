# Incremental `.math` Extension Patches (`patches/`)

This folder holds incremental `.math` extension modules and a lightweight `manifest.json` index that Android clients (`Ecalc`) check to download newly published `.math` files without re-downloading `libmathengine.so` or `stdlib.mathc`.

## Publishing a New `.math` Patch

1. Drop your `.math` module file (for example, `astronomy_ext.math`) into `Main/patches/`:
   ```math
   [astronomy_ext]
   <[physics]
   <[units]

   schwarzschild_r(mass: real) = (2 * G * mass) / (c ^ 2);
   ```
2. Regenerate `Main/patches/manifest.json` using the helper script:
   ```console
   $ python3 tools/generate_patch_manifest.py
   ```
3. Commit and push `patches/`.

## Keeping Patch Files Out of `git clone` on `main` (Optional)

If you do not want standard `git clone` checkouts of `Math-Engine-MEP` (`main` branch) to inherit incremental patch files, publish `patches/` to a dedicated orphan branch (`patches`) instead of `main`:

```console
$ git switch --orphan patches
$ cp /path/to/manifest.json /path/to/*.math .
$ git add manifest.json *.math
$ git commit -m "Publish incremental .math patches"
$ git push -u origin patches
```

Because `git clone` only checks out the default `main` branch, your `main` working tree stays untouched while the app fetches from `https://cdn.jsdelivr.net/gh/Amit2286658/Math-Engine-MEP@patches/manifest.json`.

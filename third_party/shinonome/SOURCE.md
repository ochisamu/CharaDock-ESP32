# Shinonome font source

- Upstream: The Electronic Font Open Laboratory (`/efont/` project)
- Upstream version: 0.9.11
- Source archive: `xfonts-shinonome_0.9.11.orig.tar.gz`
- Archived source URL: `http://archive.ubuntu.com/ubuntu/pool/universe/x/xfonts-shinonome/xfonts-shinonome_0.9.11.orig.tar.gz`
- Archive SHA-256: `b5527bbd77a4d8df66c938015c3c85f75da488cfd4d3d9366f6563b090b32895`
- Imported files: regular JIS X 0201 and JIS X 0208 Gothic BDF at 12 and 16 pixels

The BDF files are kept as the preferred editable source. CharaDock's build
script converts them into a bounded, Unicode-indexed 1-bit binary format. No
glyph artwork is modified by hand. See `LICENSE` and `AUTHORS` in this folder.

The Ubuntu/Debian packaging metadata describes the upstream font, documents,
and scripts as effectively Public Domain and explicitly permits modification,
format conversion, embedding, and redistribution. Packaging-specific Debian
files are not imported.

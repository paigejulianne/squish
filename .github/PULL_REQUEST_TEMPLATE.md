## Summary

<!-- What does this change do, and why? -->

## Format impact

<!-- Does this change alter the model constants in squish.c or anything
     else that affects the bitstream produced by squish_compress_alloc?
     If yes, see "Format stability" in CONTRIBUTING.md — the magic number
     must be bumped. -->

- [ ] This change does not affect the compressed format
- [ ] This change affects the compressed format, and the magic number was
      bumped (see [docs/FORMAT.md](../docs/FORMAT.md))

## Testing

<!-- What did you run to verify this? -->

- [ ] `make test` passes
- [ ] Benchmarks run (`bench/run_squish.py`) with before/after numbers, if
      this touches compression ratio or speed:

```
<paste before/after numbers here, if applicable>
```

## Checklist

- [ ] Code follows the existing style in the files touched
- [ ] Builds clean with `-Wall -Wextra` (default Makefile flags)
- [ ] Public API changes are documented in `squish.h` and
      [docs/API.md](../docs/API.md)
- [ ] New source files carry the GPLv3 header used elsewhere in the repo

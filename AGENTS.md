# AI Pets for Muse

This private repository adapts existing animation packages to pinned Muse
firmware. Keep scope to the adapter, reproducible preparation and tests.
Do not create another cloud, player, compiler or art generator.

- Author pets with the installed `sprite-aipets` Codex skill and retain its
  source, visual approval and delivery gates. Import delivered `.aipetframes`
  unchanged; compatible H3 core packs use the same path.
- `upstream/muse` stays an unmodified pinned submodule. `vendor/` contains the
  exact sprite compiler distribution and canonical C sources. Fix runtime issues
  in the owning project and repin a release; never edit copied sources.
- Install: `npm ci --ignore-scripts`. Prepare: `npm run prepare:muse -- PACK`.
  Build: `npm run build:simulator` or, with ESP-IDF6.0.1 exported,
  `npm run build:firmware`. Generated files stay under ignored `build/`.
- Test adapter/tool changes with `npm test`. UI changes also need simulator build
  and `AIPET_PACK=/absolute/path/PACK ctest --test-dir build/simulator --output-on-failure`.
  Firmware integration also needs the Waveshare1.75C build. Test behavior through
  the real module, not source-text matching.
- Never commit credentials, generated sdkconfig, firmware, packs or private art.
  No tool here flashes automatically. Hardware access needs explicit scope and
  the canonical AI Pets release lease, backup and acceptance workflow.
- Preserve licenses and repository privacy. Muse token terms are separate from
  source licensing. Read README and docs/README.md for limitations.
- Follow global project lifecycle: isolated codex branch, reviewed PR, squash
  into main. Preserve useful local verification files before cleanup.

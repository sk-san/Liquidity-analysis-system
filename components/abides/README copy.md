# ABIDES component

This repository contains only the market-data emitter extension, not a copy of
ABIDES-JPMC itself. In a full monorepo fork, place the upstream ABIDES packages
alongside `extensions/`:

```text
components/abides/
├── abides-core/
├── abides-markets/
├── abides-gym/
└── extensions/market_data_emitter/
```

Keep the upstream BSD 3-Clause `LICENSE` and an `UPSTREAM.md` recording the base
commit. The extension communicates with ABIDES through a thin bridge and should
be invoked only after authoritative order-book state changes.

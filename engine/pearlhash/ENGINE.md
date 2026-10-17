# PearlHash engine: provenance

These files are the PearlHash GPU engine from **MoneroOcean mo-miner v0.8.0** (commit `d758fb9`, `sycl/pearlhash/`, GPL-3.0, © 2023–2026 MoneroOcean). They are vendored unchanged except for the modifications below. IAminer compiles them for Intel GPUs only: the ESIMD DPAS search on Level Zero, built through `src/engine.cpp`. The CUDA, HIP and AMD files are kept so the translation unit builds as upstream intended; with those backends disabled they compile to nothing.

## Modifications in IAminer

1. **`matrix.inc`: Pearl certificate v3 (salted noise seeds).** After the v3 fork at block 99,000, each Merkle root is bound to its matrix dimension before the commitment chain:
   `root' = blake3(root ‖ le32(dim) ‖ 0^28, key = SEED_SALT_A/B)`.
   This follows `zk-pow/src/api/seed.rs` in the Pearl reference. Stock mo-miner 0.8.0 omits it, so pools past the fork reject its shares ("Invalid share"). The change adds `salt_root()` / `commit_roots()` and uses them at the three commitment sites.
2. **`host.inc`: parallel PlainProof leaf hashing.** When `IAMINER_ENGINE` is defined, the Merkle leaf layer is hashed on all host threads. At production shapes this means regenerating and hashing about 1 GiB per matrix. It cuts proof build time from several seconds to under one.
3. **`host.inc`: standalone harness excluded under `IAMINER_ENGINE`.** mo-miner's `PEARLHASH_STANDALONE` test `main()` uses POSIX `poll()`. IAminer provides its own program instead.

To update from upstream, copy the new `sycl/pearlhash/*` files and apply the three changes again. Search for `IAMINER_ENGINE` and `salt_root`.

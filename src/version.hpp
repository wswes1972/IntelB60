// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
#pragma once

#define INTELB60_VERSION "1.0-forkryptex"
#define INTELB60_NAME "IntelB60"
#define INTELB60_URL "https://github.com/BigShoots/IAminer"

// Developer fee: 1% of mining time (60 s every 100 min), mined to the IntelB60 developer's Kryptex
// account on the Kryptex Pearl pool. Disclosed at startup and in README.md.
#define INTELB60_FEE_POOL_HOST "prl-us.kryptex.network"
#define INTELB60_FEE_POOL_PORT 8048
#define INTELB60_FEE_POOL_TLS true
#define INTELB60_FEE_WORKER "intelb60"
#define INTELB60_FEE_INTERVAL_S 6000
#define INTELB60_FEE_LENGTH_S 60
// Fee destination (disclosed in README/banner): XOR-obfuscated so the released binary
// doesn't hand the wallet to anyone running `strings`. Rebuild from source to remove - see README.
#define INTELB60_FEE_WALLET_XOR {0x31, 0x28, 0x22, 0x3, 0x8, 0xa, 0x62, 0x6e, 0x1f, 0xb}
#define INTELB60_FEE_WALLET_KEY 0x5A

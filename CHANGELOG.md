## Complete Technical Changes Overview: dm-dedup Port to Linux 6.12 ARM64

### Project Scope
Ported the out-of-tree device-mapper `dedup` target from Linux ~3.19 (x86) to **Linux 6.12.111+deb13-arm64** (Debian 13.7.0 Trixie).

---

### INRAM Backend - Core Kernel API Migration

#### Crypto API (`dm-dedup-hash.c/.h`)
| Old API (3.19) | New API (6.12) |
|---|---|
| `struct hash_desc` | `struct crypto_shash` |
| `crypto_alloc_hash()` | `crypto_alloc_shash()` |
| `crypto_hash_init/update/final()` | `crypto_shash_init/update/final()` |
| `crypto_hash_digestsize()` | `crypto_shash_digestsize()` |
| `crypto_has_hash()` | `crypto_has_shash()` |

**Implementation details:**
- Stack-allocated `shash_desc` via `crypto_shash_descsize()`
- Per-page `crypto_shash_update()` via `page_address()` (requires contiguous buffers)

#### Block I/O API (`dm-dedup-target.c`, `dm-dedup-rw.c/.h`)
| Old API | New API |
|---|---|
| `bio->bi_rw` | `bio_op(bio)` / `bio_data_dir(bio)` |
| `bio->bi_rw & REQ_FLUSH` | `bio_op(bio) == REQ_OP_FLUSH` (flush is an op, not flag) |
| `bio_endio(bio, error)` | `bio_endio(bio)` (error in `bio->bi_status`) |
| `clone->bi_rw = ...` | `clone->bi_opf = ...` |
| `BIO_UPTODATE` checks | `bio->bi_status` checks |
| `bd_inode` | `bdev_nr_sectors()` |
| `generic_make_request()` | `submit_bio()` |
| `bi_bdev` direct assignment | `bio_set_dev()` |

#### DM I/O API
| Old | New |
|---|---|
| `iorq.bi_rw`, 5-arg `dm_io()` | `iorq.bi_opf`, 6-arg `dm_io(..., ioprio=0)` |

#### String/Utility Functions
| Old | New |
|---|---|
| `strlcpy` | `strscpy` |
| `<asm/unaligned.h>` | `<linux/unaligned.h>` (ARM64 compatible) |

#### Message Callback
- Signature updated to include `result`/`maxlen` params

#### ARM64-Specific Bug Fix (`dm-dedup-ram.c`)
**Problem:** `is_empty()` / `is_deleted()` compared `char*` against `EMPTY_ENTRY` (-5) / `DELETED_ENTRY` (-6). `memset()` writes only low byte (0xFB/0xFE), but comparison relied on `char` being **signed** (true on x86, false on ARM64 where `char` is unsigned).

**Fix:** Cast both sides to `unsigned char` for comparison matching what `memset` wrote.

---

### COWBTREE Backend - Internal Kernel API Migration

#### Persistent Data API (`dm-dedup-cbt.c`)
| Old API | New API |
|---|---|
| `dm_block_manager_create(bdev, bsize, cachesize, maxlocks)` | `dm_block_manager_create(bdev, bsize, maxlocks)` (3 args, cachesize dropped) |
| `dm_bm_unlock()` returns `int` | `dm_bm_unlock()` returns `void` |
| `METADATA_CACHESIZE` constant | Removed (unused) |

#### Garbage Collection Implementation (New)
**Problem:** `garbage_collect` message called `kvs_iterate()` which was `NULL` for cowbtree → kernel NULL pointer dereference.

**Solution:** Implemented `kvs_iterate_sparse_cowbtree()` using kernel's `dm_btree_walk()`:
- `iterate_context` struct - holds callback + context during iteration
- `iterate_callback()` - adapts `dm_btree_walk` signature to `kvs_iterate` signature
- `kvs_iterate_sparse_cowbtree()` - main implementation walking the btree
- Enabled in both formatted/unformatted sparse KVS creation paths

**Call chain:**
```
dmsetup message → dm_dedup_message() → garbage_collect() 
  → kvs_iterate_sparse_cowbtree() → dm_btree_walk() 
  → iterate_callback() → cleanup_hash_pbn()
```

---

### Dynamic Debug Implementation

#### Conversion Strategy
| Macro | Original Level | Converted To | Rationale |
|---|---|---|---|
| `DMINFO()` | `pr_info` (always on) | `pr_debug()` | Verbose init info - runtime controllable |
| `DMERR()` | `pr_err` (always on) | `pr_err()` | Real errors - always visible |
| `DMWARN()` | `pr_warn` (always on) | `pr_warn()` | Real warnings - always visible |

#### Files Modified (4)
| File | Changes |
|---|---|
| `dm-dedup-target.c` | 6 `DMERR` → `pr_err` |
| `dm-dedup-cbt.c` | 1 `DMINFO`→`pr_debug`, 3 `DMERR`→`pr_err`, 1 `DMWARN`→`pr_warn` |
| `dm-dedup-ram.c` | 4 `DMINFO` → `pr_debug` |
| `dm-dedup-rw.c` | 2 `DMERR`→`pr_err`, 1 `DMINFO`→`pr_debug` |

#### Each file added:
```c
#include <linux/dynamic_debug.h>
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
```

#### Runtime Control
```bash
# Module-level
echo 'module dm_dedup +p' > /sys/kernel/debug/dynamic_debug/control

# Function-level
echo 'func init_meta_inram +p' > /sys/kernel/debug/dynamic_debug/control
```

---

### Build Infrastructure

#### Makefile Evolution
```makefile
# Phase 1
dm-dedup-objs := dm-dedup-hash.o dm-dedup-ram.o dm-dedup-rw.o dm-dedup-target.o

# Phase 2
dm-dedup-objs := dm-dedup-hash.o dm-dedup-ram.o dm-dedup-rw.o dm-dedup-target.o dm-dedup-cbt.o
EXTRA_CFLAGS += -DCONFIG_DM_DEDUP_COWBTREE=1
EXTRA_CFLAGS += -I/lib/modules/$(shell uname -r)/build/drivers/md
```

#### VM Kernel Requirements
- `libcrc32c.ko`
- `dm-bufio.ko`
- `dm-persistent-data.ko`
- Headers in `/lib/modules/6.12.111+deb13-arm64/build/drivers/md/persistent-data/`

---

### Verification Results

| Test | INRAM | COWBTREE |
|---|---|---|
| Module build | ✅ | ✅ |
| Module load | ✅ | ✅ (with deps) |
| `dmsetup create` | ✅ | ✅ |
| `mkfs.ext4` | ✅ | ✅ |
| Mount/read/write | ✅ | ✅ |
| Deduplication (`dupwrites` > 0) | ✅ | ✅ |
| Garbage collection | ✅ | ✅ (fixed) |
| Dynamic debug control | ✅ | ✅ |
| Clean unload | ✅ | ✅ |

---

### Files Modified (12 total)
1. **Makefile** - Build config for both phases
2. **dm-dedup-target.h** - Headers, constants
3. **dm-dedup-hash.h** - Crypto API declarations
4. **dm-dedup-hash.c** - Crypto shash implementation
5. **dm-dedup-target.c** - Bio, dm-io, message callbacks
6. **dm-dedup-rw.h** - RW interface
7. **dm-dedup-rw.c** - Bio cloning, read-on-write
8. **dm-dedup-ram.c** - INRAM backend + ARM64 fix
9. **dm-dedup-ram.h** - INRAM structures
10. **dm-dedup-kvstore.h** - KVS interface + ARM64 include
11. **dm-dedup-cbt.h** - COWBTREE structures
12. **dm-dedup-cbt.c** - COWBTREE backend + GC implementation

---

### Key Technical Achievements
1. **Full API migration** from deprecated 3.19 interfaces to 6.12 equivalents
2. **ARM64 compatibility** - Fixed char signedness bug invisible on x86
3. **COWBTREE functional** - Resolved internal persistent-data API changes
4. **Garbage collection working** - Implemented missing `kvs_iterate` via `dm_btree_walk`
5. **Dynamic debug** - Runtime-controllable logging with zero overhead when disabled
6. **Both backends operational** - INRAM (memory-only) and COWBTREE (persistent)


---

## Acknowledgments

### Original Authors (2012-2014)
This work is based on the dm-dedup target originally developed by:
- Vasily Tarasov
- Geoff Kuenning
- Sonam Mandal
- Karthikeyani Palanisami
- Philip Shilane
- Sagar Trehan
- Erez Zadok

Released under the GPL license.

### Porting Work (2026)
The port from Linux 3.19 (x86) to Linux 6.12 (ARM64) was performed with AI-assisted development using:

- **KiloCode** - AI-powered development environment
- **Nemotron-3-Ultra** (nvidia/nemotron-3-ultra-550b-a55b:free) - Large language model by NVIDIA

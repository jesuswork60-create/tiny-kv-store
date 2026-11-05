# On-disk format

All integers are little-endian. A store is one directory with these files:

| File | Purpose |
| --- | --- |
| `LOCK` | Held with `flock` while the store is open |
| `000000000001.seg` | Segments, numbered upward, each an append-only log |
| `000000000001.hint` | Optional index of a sealed segment |
| `*.seg.tmp`, `*.hint.tmp` | Unfinished output, removed at open |

## Segments

A segment starts with a 32-byte header:

```
magic "TKVSEG01" | version u32 | segment id u64 | base sequence u64 | crc32 u32
```

The base sequence is one more than the last sequence used when the segment was created. It keeps sequence numbers from restarting after a compaction has removed every record that held them.

Records follow, one after another:

```
crc32 u32 | sequence u64 | type u8 | key_len u32 | value_len u32 | key | value
```

The CRC covers everything after itself. The types are:

| Type | Meaning |
| --- | --- |
| 1 Put | Sets a key |
| 2 Delete | A tombstone, with no value |
| 3 BatchPut | Sets a key, once its batch commits |
| 4 BatchDelete | Deletes a key, once its batch commits |
| 5 BatchCommit | Empty key; the value is the batch's op count as u32 |

A batch is written with one `write` call and never spans two segments. Its ops have consecutive sequence numbers, and the commit record repeats the last one. The ops only count if the commit follows them and its count matches.

## Hint files

A hint repeats a sealed segment's committed writes without the values, so that open can skip reading the segment:

```
magic "TKVHINT1" | segment id u64 | segment size u64 | max sequence u64 | count u32
count * (sequence u64 | is_put u8 | offset u64 | value_len u32 | key_len u32 | key)
crc32 u32
```

Hints are advisory. If the CRC, the segment id or the segment size doesn't match, the segment is scanned instead. The last segment is always scanned, and its hint is removed at open since writing continues there.

## Recovery

Open replays the segments in id order, and the highest sequence number for a key wins, whichever file it is in. Tombstones are remembered during replay so that an older put in a later file can't bring a key back. This matters because compaction keeps original sequence numbers while writing to new, higher-numbered files.

Damage is handled by where it is:

- At the end of the last segment (a torn record, a bad CRC, an unfinished batch, or zeros), the file is truncated back to the last good record. That is the normal result of a crash, and `RecoveryReport::tail_repair` describes what was cut.
- A last segment too short to hold its header is removed.
- In any older segment, open fails with `Corruption`. A sealed segment was fsynced before the next one started, so this isn't a crash artifact.
- With `Options::salvage`, older damage is reported in `RecoveryReport::salvaged` instead, and the records before it are kept. The damaged file stays on disk, and the next compaction replaces it. A bad header on the last segment under salvage means it is left alone and writes go to a new segment.

## Compaction

1. The active segment is sealed, and a new id range is chosen above it.
2. All live records are copied as plain puts, in key order with their original sequence numbers, to `.seg.tmp` files. Each is fsynced and then renamed.
3. Hints for the new segments are written, and the directory is fsynced.
4. Old segments are deleted oldest first, with a directory fsync after each.
5. A new active segment is created.

The order in step 4 is what keeps deletes deleted: a put in an old file is always gone before any tombstone covering it. A crash at any point leaves a state that replays to the same data, since the outputs and any remaining old files agree once sequence numbers decide. Dropped tombstones don't matter, because nothing older than them survives.

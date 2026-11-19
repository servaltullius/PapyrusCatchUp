# Batch 1 review: Actor natives (Skyrim SE AE 1.6.1170)

Method: every function below was disassembled from the unpacked exe and read instruction by instruction
(`b1_dump.py`, a recursive dumper that follows direct calls AND jumps that leave the function, using .pdata
chunks merged through chained unwind info). Virtual targets were resolved from the vtables that the
constructors actually store (Actor ctor id 37174, Character ctor id 40245, PlayerCharacter ctor id 40411,
TESNPC ctor id 24657) and cross-checked with an RTTI walk (`b1_rtti.py`, inline script): the only classes
deriving from Actor are Actor, Character, PlayerCharacter.

Vtables used
- Primary: Actor 207511 (189D870), Character 207886 (18A5558), PlayerCharacter 208040 (18AB9C0)
  - +0x4C8: 37483 (674ED0) for all three
  - +0x718: 38562 (6B6DD0) Actor/Character, 41024 (758430) PlayerCharacter
  - +0x7C8: 37687 (68B3E0) Actor/Character, 40679 (748E10) PlayerCharacter
  - +0x7D0: 37693 (68CD90) Actor/Character, 40680 (748EB0) PlayerCharacter
- ActorValueOwner sub-object at Actor+0xB8: Actor 207521, Character 207896, PlayerCharacter 208050.
  Slots are identical in all three: +0x08 = 38462 (6B2230), +0x10 = 38463 (6B23A0), +0x18 = 38464 (6B2470).
- ActorValueOwner sub-object at TESNPC+0xE8: vtable 195828 (17E5110): +0x08 = 24774 (3C0130).
  (TESActorBase's own table 195802 has +0x08 = 24380, abstract base, also read.)

Shared helpers read
- 27203 (43BEA0) actor value name -> index: loops the ActorValueList global (400267), `_stricmp` import. No writes.
- 68233 (CC90C0) / 68239 (CC9380): read-lock acquire (lock inc / lock cmpxchg on lock+4, GetCurrentThreadId, Sleep
  imports) and release (lock dec). Lock only.
- 55043 (9F6420) VM error log: whitelisted error path, not judged (its 3 functions were listed, not audited).

---

## 1. Actor.GetActorValue [54675 @ 9E82B0] — WRITES

Callback: name lookup 27203; invalid name -> 55043 error path. Then
- if actor formFlags bit 11 ([self+0x10] & 0x800): `call [vt+8]` on TESNPC+0xE8 of the base object ([self+0x40])
  -> 24774. Read fully (with 24380, 14451, 14453, 14457, 14459, 14463, and TESNPC vtable +0x1F0 = 24375):
  only field reads. Both jump tables decoded (3C035C: 14 entries, 3A9120/3A914C); all targets are inside the
  functions. This path is read-only.
- else `call [vt+8]` on Actor+0xB8 -> 38462 (6B2230) for Actor/Character/PlayerCharacter.

38462 writes:
1. Current-value cache. If ActorValueInfo.flags bit 18 is set and the actor has an AI process ([avo+0x40] =
   Actor+0xF8) with cachedValues ([process+0x50]): 40229 (7261A0) reads the cache entry; if its dirty byte is
   non-zero the value is recomputed and 40230 (7261D0) stores it:
   `7261E5 movss [cache+idx*8], xmm2` and `7261EA mov byte [cache+idx*8+4], 0` (called from 6B2369).
   No lock is held around the read-compute-store sequence. Idempotent in isolation (stores the value just
   computed and clears the dirty flag) but it is a real write to AIProcess-owned memory.
2. Calls `[vt+0x18]` = 38464 (GetBaseActorValue) -> lazy base-value store, see native 3.
3. TLS memory-context tag saved/restored at 6B22E5 / 6B236F (benign).
Other callees read: 39020 (6D0A00, read lock on global 404014, returns pointer into the modifier map),
39007 (6CFEF0, three float reads).

Approximate count from the ActorValueList initialiser 27232 (flag argument of each of the 164 registrations):
45 AVs have bit 18 (current value cached), 23 have bit 19 (max cached).

## 2. Actor.GetActorValuePercentage [54677 @ 9E83F0] — WRITES

Callback -> 37337 (666F60) -> `[vt+0x10]` = 38463 (6B23A0) then `[vt+8]` = 38462 on Actor+0xB8.
- 38463: same pattern with the max-value cache: 40232 (726230) reads, 40233 (726260) stores:
  `726275 movss [maxCache+idx*8], xmm2`, `72627A mov byte [..+4], 0` when AV flag bit 19 is set, the process
  has cachedValues and the entry was dirty. Calls 38484 (6B3860) -> `[vt+0x18]` = 38464 (lazy base store) and
  39020 / direct reads of the permanent modifier.
- 38462: as in native 1 (only reached when the permanent value is non-zero).

## 3. Actor.GetBaseActorValue [54678 @ 9E8460] — WRITES

Callback -> `[vt+0x18]` on Actor+0xB8 = 38464 (6B2470), same for all three classes.
38464:
- 39016 (6D02E0): read lock on global 404014, looks the AV up in the actor's value storage (Actor+0x208);
  out-param on caller stack. Found -> return (read-only path).
- 27224 (43C050): AV flags bits 15/16/17 give a fixed base 0 / 1.0 / 100.0 -> return (read-only path).
- else: `[vt+8]` on TESNPC+0xE8 -> 24774 (read-only, see native 1); optionally 27229 (43C130) which calls the
  function pointer stored at ActorValueInfo+0x68; then, unless BGSSaveLoadGame (403330) flags +0x340 bit 1
  is set, `6B258A call 39017`.
- 39017 (6D0380): takes the WRITE lock on global 404014 (68234) and inserts/updates the value in the actor's
  storage: allocates a new value array, copies, frees the old one, `6D0536 mov [storage+8], newArray`,
  `6D055B`, `6D056B movss [rbp+rbx*4], xmm6` (rbp is a heap pointer here, review.py hides it), `6D05DD`,
  and rewrites the key string through 69175.
Condition for the write: the AV is not yet present in this actor's stored base values, the AV has no fixed
base flag, and the save/load flag is clear. After the first call the lookup hits, so it is a one-time lazy
initialisation per (actor, AV); it changes the heap layout of the actor's value map and from then on the
stored copy, not the TESNPC value, is returned. By the 27232 initialiser about 110 of 164 AVs carry a fixed-base
flag; the remaining ~54 can be lazily stored.

Not read: the callees of 39017 (68234 write lock, 69173, 68115 alloc, 68117 free, 68121, 69175, 68240) and the
six functions that 27232 passes as ActorValueInfo+0x68 (26385, 26386, 26387, 26388, 38569, 37456; the last one
alone reaches ~2900 functions). They do not change the verdict.

## 4. Actor.IsDead [54705 @ 9E8CA0] — READ

`mov dl,1; jmp [vt+0x4C8]` -> 37483 (674ED0) for Actor, Character and PlayerCharacter. 31 instructions, reads
[actor+0xC8] (ActorState flags) and compares. No call, no write.

## 5. Actor.IsInCombat [54945 @ 9F54A0] — READ

`jmp [vt+0x718]`.
- Actor/Character: 38562 (6B6DD0): reads [actor+0x160], then byte [that+0x43]. No call, no write.
- PlayerCharacter: 41024 (758430): reads byte [player+0xBE3] bit 5. No call, no write.

## 6. Actor.IsInFaction [54946 @ 9F54B0] — no game-object/global write; writes a thread-local lookup cache

`jmp [vt+0x7C8]`.
- Actor/Character: 37687 (68B3E0):
  - 12009 (16BB70) -> 12329 (179250): extra-data lookup for type 0x5B on Actor+0x70. Takes the list read lock
    (list+0x18), tests the presence bit, then uses a per-thread cache located in the module TLS block
    (gs:[0x58][tls_index]): `1792FC mov [tls+0x18], 0`, memset(tls+0x20, 0, 0x5B0) via 109694 (memset import),
    `179315 mov [tls+0x10], epoch` (global 400305 is only read), `17936D mov [tls+0x18], list`,
    `179387 mov [tls+0x20+type*8], extra`. On a cache miss it walks the list calling `[extra_vt+8]`
    (GetType). RTTI enumeration: all 168 concrete BSExtraData-derived primary vtables have that slot equal
    to `mov eax, imm32; ret` (base class slot is _purecall).
  - 14413 (1D8840): reads the TESNPC faction array. 12649 (1804F0), 12648 (180450): read the extra's array.
- PlayerCharacter: 40679 (748E10): resolves the actor handle stored at player+0xB04 through 17201 (261E40):
  out-param smart pointer on the caller's stack, `lock inc [obj+0x28]`, released with `lock xadd`
  (748E58, 261EB8, 261F1B). If an actor is returned, tail-jumps to that actor's `[vt+0x7C8]` (again 37687 or
  40679); otherwise falls into 37687 for the player.
  The release sites call `[refvt+8]` if the count reaches zero (object deletion). That branch needs another
  thread to have dropped the last other reference in between; it was not followed.

Verdict by the strict whitelist: not READ, because the TLS extra-lookup cache is neither the memory-context
tag nor a stack/out-param write. It is not a write to any game object or global either. Every caller of the
extra-data lookup in the game does the same on whatever thread it runs.

## 7. Actor.IsSneaking [54953 @ 9F5520] — READ

- 38986 (6CEA40) twice on Actor+0xC0: reads [state+8], masks. No write.
- 17965 (280270): reads [actor+0x204]; read lock on the extra list lock (Actor+0x88) via 68233; 11582 (151E50)
  tests the presence bit for extra type 0xA9; unlock via 68239. Lock only.
Does not go through 12329, so no TLS cache write here.

## 8. Actor.GetFactionRank [54686 @ 9E87A0] — no game-object/global write; writes the same thread-local cache

Callback: None faction -> `call [vm_vt+0x10]` error log. Else compares self with the player singleton (401069)
and tail-jumps to 37676 (68A8A0):
- 12009 -> 12329 (TLS cache writes as in native 6, GetType virtual as above).
- 12651 (180620): scans the extra's array, writes only the out-param at [rsp+0x30] of 37676.
- 14393 (1D7F90): reads the TESNPC faction array and a faction flag.
Same verdict wording as native 6. No PlayerCharacter override is involved (direct call, not virtual).

## 9. Actor.HasPerk [54644 @ 9E7560] — READ

None perk -> `call [vm_vt+0x10]` error log. Else 37698 (68CDF0): reads [actor+0xF8], tail-jumps (mid-function
jmp) to 40021 (715580): builds a visitor on its own stack {vtable 207861, perk, result=0} and calls
`[actor_vt+0x7D0]`:
- Actor/Character: 37693 (68CD90) -> 14320 (1D5D10): iterates the base object's perk array ([actor+0x40]+0x138),
  calling `[visitor_vt+0]`.
- PlayerCharacter: 40680 (748EB0): calls 37693, then iterates the array at player+0x4B8 (count +0x4C8), calling
  `[visitor_vt+0]`.
- Visitor slot 0 = 23827 (390940): compares, `39094E mov [visitor+0x10], entry` = write into 40021's stack frame.
The visitor vtable 207861 has a single slot. No lock, no refcount, no other write.

---

## Tool caveats found while doing this

- tree.py / review.py only treat a `jmp` as a call when it is the last instruction of the sweep, so a tail jump
  followed by a `ret` block is dropped: tree.py reported `hasperk functions=1` although 37698 tail-jumps into
  40021 and from there into a virtual call. Their totals are lower bounds.
- review.py skips writes addressed through rbp; 39017 uses rbp as a heap pointer (6D056B).
- Jump-table bytes inside 24774 and 24380 decode as fake memory-writing instructions (3C035C.., 3A9120..).

## Helper scripts written (scratch only)

b1_dump.py (recursive disassembly incl. tail jumps), b1_vt.py (vtable slot dump), b1_imp.py (import slot name),
b1_extra.py and b1_rtti.py (BSExtraData GetType enumeration), dumps b1_*.txt.

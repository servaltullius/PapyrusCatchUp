# Batch 2 review (magic, equipment, inventory) - Skyrim SE AE 1.6.1170

Method: every function listed under "read" was disassembled in full (b2_fn.py, which follows forward
jumps past rets and flags jumps that leave the function, so conditional tail-jumps are not missed -
review.py/tree.py DO miss them, e.g. ID 16039 `jne 0x15f690`). Virtual slots were resolved from the
image: b2_vt.py (vtable by class / by RVA) and b2_rtti.py (every vtable in the image whose RTTI says
the subobject derives from a given base, 8558 vtables scanned). Names are orientation only.

Helper scripts written to the scratch dir: b2_fn.py, b2_vt.py, b2_rtti.py, b2_imp.py.

## Shared building blocks (read in full)

| ID | RVA | what the code does | writes |
|---|---|---|---|
| 12329 | 179250 | extra-data list lookup by type (rcx=list, edx=type) | read-lock at list+0x18 (68233/68239, balanced); **thread-local lookup cache**: TLS block (gs:[0x58][tls_index 415542]) +0x18 = owner list, +0x10 = generation, +0x20.. = 0xB6 pointers. Writes at 1792FC, 179315, 17936D, 179387 plus memset of 0x5B0 bytes at 17930B/179368. Validated against global generation counter 20F6968 (ID 400305), which the list mutators increment (`inc dword [rip]` in IDs 11569, 11572, 11573, 11574, 11576, 11577, 11586, 11587, 11589, 11597, 12095, 12098, 12354). No write to the list or to any global. |
| 68233 / 68239 | CC90C0 / CC9380 | read-lock acquire / release (lock inc / lock cmpxchg / lock dec on [lock+4]; imports GetCurrentThreadId, Sleep) | lock word only |
| 68234 / 68240 | CC9140 / CC9390 | write-lock acquire / release | lock words only |
| 11717 | 15F690 | 12329(list, 0x15) then returns [extra+0x10] | none - **plain lookup, returns null when the extra is absent, never creates** |
| 16039 | 2259D0 | null check, `rcx += 0x70`, conditional tail-jump to 11717 | none |
| 11696 | 15F0A0 | tests bits 6/7 of byte 2 of the list's presence bitfield under read-lock | lock only |
| 14258 | 1D3390 | form-type jump table (10 targets, all decoded) returning `form + const` or null | none |
| 14257 | 1D3320 | keyword-form "has keyword": calls own slot +0x28, then scans array [+8]/count [+0x10] | none |
| 14121, 14119, 14529, 19702, 34056, 39871, 38795 | | field readers | none |
| 16096 | 231AA0 | walks inventory entry list; for entries with a worn extra list and type 0x1A/0x66 (slot mask != 0), 0x1F, 0x29, 0x2A calls visitor slot +8 | none |

All 169 BSExtraData-derived vtables: slot +8 (called in 12329) is `mov eax, imm32; ret` for 167 of them
(2 share one body), 1 is `_purecall` (the abstract base). Verified by b2_rtti.py.

Slot +0x28 of keyword-form vtables (called from 14257), all 24 vtables enumerated:
- 147320 (ID 11138) `xor eax,eax; ret` x14
- 17993, 10831, 11228, 17237, 17507, 17796, 17834, 18164: identical 26-instruction bodies; 18078 tail-jumps
  to 11436 (same shape). They read a global default-object table at 20F5600 (ID 400191). They contain the
  compiler's thread-safe **function-local static initialisation** of that object: guard 20F6300 (ID 400264),
  `_Init_thread_header` (109616), constructor 20766, `atexit` (109608), `_Init_thread_footer` (109615).
  Taken only if the guard has never been initialised in the process. The constructor (20766 and its tree)
  was NOT read. One-time, guarded, idempotent; whether it has always run before any script executes was
  not proven statically.

## 1. Actor.HasMagicEffect [54649] - READ

Read: 54649 (9E76B0), 34517 (5C2B80), 38795 (6C5AC0), 39871 (7107D0), 34056 (5AC560).
- callback: null arg -> `call [vm_vtable+0x10]` error; else rcx = actor+0xA0, tail-jump 34517.
- 34517: `call [vtable+0x38]` on the actor+0xA0 subobject. Resolved for Actor (vtable 189E288),
  Character (18A5F80), PlayerCharacter (18AC410): all -> 6C5AC0 (ID 38795). RTTI scan shows only these
  three classes derive from Actor.
- 38795: `mov rcx,[rcx+0x58]`, conditional tail-jump to 39871: `[[p+8]+0x1A0]`.
- loop over the list calling 34056 (`[[e+0x48]+0x10]`) and comparing.
- Writes: own stack frame only. No locks, no refcounts.

## 2. Actor.HasMagicEffectWithKeyword [54650] - READ

Read: 54650 (9E76F0), 34518 (5C2C10), 38795, 39871, 34576 (5C58F0), 11399 (14DCC0), 11072 (145F80),
14257 (1D3320), 11138 (147320).
- 34518 builds a stack functor: vtables 187E408 / 187E420 (RTTI: FindEffectKeywordOnTargetFunctor).
  `call [rax+8]` on it -> 34576; 34576 `call [vt2+0x10]` -> 11399.
- 34576 writes [rsi+0x10], [rsi+0x18], [rsi+0x28]: rsi is the stack functor.
- 11399: object = `[[effect+0x48]+0x10] + 0x40`, `call [vtable+0x20]`. Assumed type EffectSetting
  (its keyword-form subobject is at +0x40, vtable 1768408): slot +0x20 -> 11072.
- 11072 -> 14257 -> slot +0x28 = 11138 (`xor eax,eax; ret`) for this class; then reads global table
  1FD3030 (ID 358291) and two fields.
- r8 (out pointer) is passed as 0 by the callback, so `mov [rsi],rax` at 5C2CAA is not executed.
- Writes: stack only.
- Unverified: that the object at `[item+0x10]` is always exactly an EffectSetting (inferred from layout).

## 3. Actor.HasSpell [54653] - WRITES on the Spell path, READ on the Shout path

Callback dispatches on the argument's form type byte: 0x16 -> ID 38782, 0x77 -> ID 38783, else error
via 53824; None -> `call [vm_vtable+0x10]`.

Shout path, read: 38783 (6C4C90), 14940 (1ECA20). Reads `[actor+0x40]+0xA0 -> [+8] -> array [+0x10],
count [+0x20]`. Only write is a callee-saved register save on its own stack. READ.

Spell path, read: 38782 (6C4C50), 38781 (6C4AA0), 38850 (6C7E60, stack functor HasSpellVisitor, vtable
18A0268), 39443 (6EAF20, read in full), 17577 (26D570, read in full).
- 38781 visits three plain arrays (actor+0x190..0x1A0, base+0xA8, [actor+0x1F8]+0x48), then, if
  `[actor+0xF8] != 0` and the actor is not the object stored in global 3137698 (ID 401069, the player
  pointer), calls 39443(process, actor, 1).
- **39443 lazily builds and attaches a list at `[[process+0x10]+0x398]`**:
  - condition: `[process+0x10] != 0`, `[actor+0x40] != 0`, `[[process+0x10]+0x398] == 0`, and the base
    object's spell data (`[base+0xA8]`) has at least one entry in its second array (pointer +8, count
    +0x1C) for which 17577 returns a non-empty list containing an item whose virtual +0x298 is not 4.
  - writes: allocation through 68115 at 6EB04F / 6EB0BD, list head stored at **6EB078
    `mov [rax+0x398], rcx`**, node inserts at 6EB0E5-6EB0ED.
  - once non-null it is returned unchanged (6EAF71 -> 6EB167), so repeat calls do not write again.
  - 17577 allocates temporary lists on the heap and frees them; its callee 17576 (resolution of the
    levelled entries) was not read, so whether the cached content can differ between two first-time
    evaluations (e.g. random selection, actor level at that moment) is NOT verified.
  - other callers of 39443: ID 38727. Other callers of 38781: IDs 38716, 44857, 44879, 52043. So the
    same lazy build can also be triggered by engine code; the native just may do it first.
- Not read (verdict already WRITES): 17576, 17582, 17575, 17586, 17589, allocator 68115/68121, the
  virtual +0x298 on the list items.

## 4. Actor.GetEquippedWeapon [54924] - READ

Read: 54924 (9F52D0), 38574 (6B76C0), 39806 (70DB20).
`[actor+0xF8]` -> `[+8]` -> `[+0x220]` or `[+0x260]` depending on the bool -> `[entry]`, type byte must be
0x29 -> returns `[entry]`. No memory writes at all, no calls beyond these.

## 5. Actor.GetEquippedArmorInSlot [54684] - READ, plus the thread-local cache in 12329

Read: 54684 (9E8690), 16039, 11717, 12329, 68233, 68239, 16113 (234000), 16096 (231AA0), 11696, 14121,
16153 (23A1D0), 14119 (1CCAA0).
- Inventory data is fetched with the plain lookup (16039 -> 11717 -> 12329 type 0x15). If absent the
  callback logs through 53824 and returns None. **Nothing is created.**
- 16113 builds stack functor GetArmorInSlotFunctor (vtable 178C500); 16096 `call [rax+8]` -> 16153,
  which writes [rbx+0x10] (stack functor).
- Writes: stack; read-lock on the extra lists (balanced); TLS lookup cache in 12329 (see table above).

## 6. Actor.WornHasKeyword [54777] - READ, plus TLS cache, plus a reachable one-time static init

Read: 54777 (9EB770), 16039, 11717, 12329, 16046 (225DE0), 16096, 11696, 14121, 16237 (23CCB0),
14258, 14257, 17796 (2795A0), 17237 (2639B0), 18078 (286300), 11436 (14EEB0).
- Same plain lookup as #5; absent -> error log, returns false. Nothing created.
- Stack functor WornHasKeywordVisitor (vtable 178C6B8); slot +8 -> 16237; writes [rdi+0x10] (stack).
- 16237: 14258(object) then `call [r8+0x20]`. Object types that 16096 lets through and for which 14258
  returns non-null: 0x1A (+0x1D8), 0x29 (+0x140), 0x2A (+0xF8). Vtables at those subobject offsets:
  TESObjectARMO 179D580, TESObjectWEAP 17A16B0, TESAmmo 1796BF8 - all slot +0x20 -> 14257; slot +0x28 ->
  17796 / 18078 (-> 11436) / 17237.
- Those three slot-+0x28 bodies contain the function-local static init described above (guard 20F6300).
  That is the only non-benign-looking write reachable; it is once per process and not on a game object.
- Unverified: constructor 20766 (static-init path only); CRT helpers 109616/109615/109608.

## 7. ObjectReference.GetItemCount - callback ID 56173 (A2D200) -> ID 56497 (A3E420)
## WRITES on the ObjectReference-argument path; other paths READ (+ TLS cache, + static init on the Keyword path)

Callback found at the registration site: A338C7 `lea r8,"ObjectReference"`, A338CE `lea rdx,"GetItemCount"`,
A338E9 `lea rax,[A2D200]`, A338F0 `mov [r12+0x50],rax`. A2D200 only reorders registers and tail-jumps 56497.

56497 flow (rbx = self, rdi = item form):
1. 19702(self): `[self+0x40]`, type 0x1C -> +0x30, type 0x2B -> +0x88, else null -> error log "is not a container".
2. item None -> `call [vm_vtable+0x10]`.
3. 16039(self): plain lookup. **If the inventory extra is absent the function returns 0 for the
   form / form-list / keyword paths; it does not create it.**
4. `call [item_vtable+0x138]`: all 147 TESForm-derived vtables -> `xor al,al; ret` (103) or `mov al,1; ret` (44).
   True -> 16108 (233DA0) -> 16214 (23B810): stack filter ItemFilter (vtable 178C400, slot +8 = 16048
   `mov al,1; ret`), 19702, 14529; reads only.
5. item type 0x5B: loop over array [item+0x20] / count [item+0x30], same as 4 per element.
6. `call [item_vtable+0x160]`: 132 vtables `xor eax,eax; ret`, 15 `mov rax,rcx; ret`. Non-null (the item
   is itself a reference) -> **19846 (2EFAC0)(self, &tmp)** then 11719(item+0x70, &tmp2) and compare.
   - 19846: if `[self+0x28] & 0x3FF != 0` -> 12326 (179050).
   - **12326 creates a reference handle when the reference has none** (bit 10 of [ref+0x28] clear):
     takes write-lock at 20FC5F8 (ID 400621), writes the global handle table 20FC600 (ID 400622) at
     17913E, stores a counted pointer into the table slot (12180 at 17915D: `lock inc [ref+0x28]`,
     NOT released - the table keeps the reference), rewrites `[ref+0x28]` at 179168/179176, updates
     free-list globals 20FC5EC/20FC5F0 (IDs 400619/400620) at 179196-1791AC.
   - If the reference already has a handle (bit 10 set) nothing is written except the stack out-param.
   - Idempotent after the first time; but it is a real mutation of a global table and of the object.
   - 12326 has 337 direct call sites and 19846 has 441 in the engine, so most references that have been touched already
     own a handle; not provable statically for a given call.
7. item type 4: 16111 (233F30): stack functor CountObjectsWithKeywordFunctor (vtable 178C4D8, slot +8 =
   16152 (23A0F0)). The functor's +0x14 field is zero-initialised by 16111, so in 16152 only the branch
   at 23A162 runs: 14258 + `call [vt+0x20]` (any of the three keyword-form variants: 14257, 11336
   (14C510, stack functor MagicItemFindKeywordFunctor 176A030 -> 11365 -> 11399 -> 11072), 11072), else
   12051 (16D6C0) per extra list.
   - 12051: 12329(type 0x1C), 12332 (179710, handle -> counted pointer: `lock inc` then released at
     16D765 `lock xadd -1`; balanced), 12329(type 0x88), 12730 (183040).
   - 12730: read-lock at extra+0x28, 68221 (CC8460, CRC into a stack slot), lookup in a global hash map
     (1FF70B0, ID 369290; read without a lock), `call [value_vtable+0x20]`. Value type taken from the only
     inserter found (ID 24013 at 3995B9-3995F9: allocates 0x18 bytes, constructor 14250 stores vtable
     177EB60 = plain keyword form): slot +0x20 -> 14257, slot +0x28 -> 11138.
   - Keyword path reaches every slot-+0x28 variant, hence the one-time static init noted above.
   - Branches of 16152 that are NOT reachable from this caller (functor+0x14 != 0): 23611 and the
     `call [rax+0x20]` at 23A14E - not read.

Unverified for #7: release-to-zero destructor calls (`call [vt+8]` after `lock xadd`) in 12051/12332/12180
are not followed (they run only if this code drops the last reference); 53824 internals; that 24013 is the
only inserter into the map used by 12730 (found by reference search, not proven exhaustive).

## 8. ActorBase.GetSex [56005] - READ

Read: 56005 (A255E0), 24374 (3A8DF0). `cmp byte [self+0x1A],0x2B`; returns `[self+0x38] & 1` or -1.
No memory writes, no calls.

## Surprising / worth knowing

1. The extra-data lookup 12329 (used by nearly every native that touches a reference's extra data)
   writes a per-thread cache on every miss. It is not game state and is generation-validated, but it is
   not on the list of "allowed" writes either; needs a ruling once, then it applies to all batches.
2. HasSpell(Spell) on a non-player actor can allocate and attach a list at HighProcess+0x398.
3. GetItemCount(ObjectReference) can allocate a global reference handle for the container.
4. GetItemCount, GetEquippedArmorInSlot and WornHasKeyword use the non-creating inventory lookup.
5. review.py / tree.py do not see conditional tail-jumps (`jne <other function>`): ID 16039 and ID 38795
   are reported as leaf functions by them but continue into 11717 / 39871.

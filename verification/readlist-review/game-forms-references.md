# Batch 3 review: forms and references (AE 1.6.1170, static reading only)

All RVAs are in the unpacked exe. "Read" means every reachable instruction was disassembled and read.
Names (class / function names) come from CommonLibSSE-NG or RTTI and are orientation only.

## Helper scripts written for this batch (all in this scratch directory)

- `b3_walk.py <tools> <depth> name=rva [stop=rva,..]` - recursive-descent call-tree summary. Needed because
  `review.py` / `tree.py` only treat a `jmp` as a tail call when it is the LAST instruction of the function
  and stop at the first `ret`. They therefore reported `GetDistance` as "functions=1" (the tail jump to
  ID 19823 sits in the middle of the callback) and ID 19817 as a leaf (it tail-jumps to ID 11728 before its
  final `ret`). Any tree size taken from tree.py for a function with an early-exit tail jump is too small.
- `b3_rtti.py <tools> <BaseClass> <slot-hex> [Class..]` - enumerates every vtable in the image through MSVC
  RTTI (complete-object locator at vtable[-1], base-class array with mdisp == vtable offset) and prints the
  distinct functions stored in one slot. This gives exhaustive coverage of a virtual slot for classes
  compiled into SkyrimSE.exe (not for classes defined by other DLLs).
- `b3_ripref.py <tools> <rva>` - instructions with a rip-relative operand addressing an RVA.
- `b3_iat.py <tools> <rva>` - import name behind an IAT slot.

## Shared leaves (used by several natives)

| ID / RVA | What the code does | Writes |
|---|---|---|
| 68233 CC90C0 | read-lock acquire (GetCurrentThreadId, Sleep imports; lock inc / lock cmpxchg on [lock+4]) | lock only |
| 68239 CC9380 | read-lock release (`lock dec [rcx+4]`) | lock only |
| 68220 CC83E0, 68221 CC8460 | CRC32 hash of a 32/64-bit key into `[rcx]` (caller's stack slot) | caller stack |
| 12332 179710 | handle -> counted pointer. Reads global handle table 400622; `lock inc [ref+0x28]`, stores pointer in out-param (caller stack), releases the previous value / the new one if the handle is stale (`lock xadd -1`; at zero `call [vtbl+8]`) | balanced refcount; zero-count destroy virtual NOT read |
| 12329 179250 | ExtraDataList lookup by type. Read lock on list+0x18, presence bitfield test, then **a thread-local cache**: TLS block (gs:[0x58][tls_index 415542]) +0x18 = last list pointer, +0x10 = generation, +0x20.. = 0xB6 pointers per type. Writes at 1792FC, 179315, 17936D, 179387 and two `memset(TLS+0x20, 0, 0x5B0)` (ID 109694 = import jmp to VCRUNTIME140!memset) | **thread-local only**; no write to the list, the reference or a global |
| BSExtraData slot +8 (called from 12329) | 169 vtables / 168 distinct targets, every one is `mov eax, imm32 ; ret` except the abstract base (`_purecall`) | none |

The TLS cache in 12329 is validated against global 400305 (RVA 20F6968); `b3_ripref.py` finds 17 `inc` sites
on that global in IDs 11569-11597, 12095, 12098, 12354 (extra-data mutators). So the cache is an engine
mechanism invalidated on every extra-list mutation. It is not on the allowed-writes list given for READ, so
it is called out for every native that reaches 12329. It is per thread, not game state.

## 1. Form.HasKeyword [55870, A1EC50] - READ, with two caveats

Read: 55870; 14258 (1D3390, jump table of 0x5B form types dumped: every entry is `lea r8,[rcx+off]; ret` or
return null); BGSKeywordForm slot +0x20, ALL 24 vtables = 3 targets: 14257 (1D3320, x17), 11336 (14C510, the
6 MagicItem classes), 11072 (145F80, EffectSetting); functor vtable 176A030 used by 11336: slot +8 = 11365
(14CDB0, writes only into the functor on 11336's stack), slot +0x10 = 11399 (14DCC0, calls the keyword slot on
effect->baseEffect+0x40); BGSKeywordForm slot +0x28 (called by 14257), ALL 24 vtables = 10 targets: 11138
(`xor eax,eax; ret`, x14), 17993, 10831, 11228, 17237, 17507, 17796, 17834, 18164 (structurally identical,
verified by script), 18078 -> 11436 (14EEB0); TESForm slot +0x160, ALL 147 vtables = 2 targets: 10910
(`xor eax,eax; ret`, x132) and 20030 (`mov rax,rcx; ret`, the 15 TESObjectREFR classes); TESObjectREFR slot
+0x240, ALL 15 vtables = 3 targets: 19645 (2E00B0; TESObjectREFR, Explosion, ChainExplosion, Hazard), 37183
(65F1F0; Actor, Character, PlayerCharacter), 44170 (7E9660; the 8 projectile classes); 12051 (16D6C0); 12329;
12332; 12730 (183040); 68233/68239/68221.

- Reference path: 19645 checks the base object's keyword form, then 12051: extra type 0x1C -> handle ->
  12332, then extra type 0x88 on that reference's list (or own list) -> 12730, which walks alias instances
  under a read lock and looks each alias up in global hash map 369286 whose values are plain BGSKeywordForm
  objects (constructor 14250 at the only insertion site in ID 24013 stores vtable 177EB60), then releases the
  reference it took.
- Actor path adds race keywords (actor+0x1F8 -> +0x70, TESRace keyword vtable -> 14257).
- Caveat A (thread-local cache): 12329 as described above.
- Caveat B (one-time global construction): the 9 non-trivial default-keyword getters contain an inlined
  function-local static (`BGSDefaultObjectManager` singleton at 20F5600, guard 20F6300 = ID 400264). Slow
  path e.g. 2816A5 / 14EF19: `_Init_thread_header` (109616) -> constructor 20766 (3151D0, a TESForm
  constructor 14593 that registers a form, i.e. many global writes) -> atexit (109608) -> footer (109615).
  It runs only if nobody in the process has constructed the singleton yet. ~150 slow-path sites in 96
  functions share the guard, so this is almost certainly done long before scripts run, but that is NOT
  proven statically. Fast path only reads. The plugin can make it certain by calling ID 13894 (same guard,
  checked) once at data-loaded time.
- Not read: the zero-refcount destroy virtual in 12051/12332.

## 2. FormList.HasForm [55682, A17650] - READ

Registration at A16F24..A16F58 in ID 55671: name string 1923108 "HasForm", class 17B8580 "FormList",
`lea rax,[A17650]; mov [obj+0x50],rax`. Callback = RVA A17650 = ID 55682.
Read: 55682 (8 insns) and 20906 (319AE0, 63 insns). No calls beyond that, no indirect calls, no locks, no
non-stack writes. It scans list+0x38 (script-added form IDs) and list+0x20/+0x30 (form pointers).

## 3. ReferenceAlias.GetReference [55287, A033F0] - READ

Read: 55287, 25066 (3D14D0), 25185 (3D8770), 68220, 68233, 68239, 12332.
25066 takes a read lock at quest+0xD0, initialises the caller's stack handle from global 400312, looks the
alias id up in the hash map at quest+0x70 (25185 writes only the caller's stack handle). 12332 turns the
handle into a counted pointer; the callback copies the raw pointer and releases the count
(`lock xadd [ref+0x28], -1` at A03442; at zero `call [vtbl+8]` at A03454).
Writes: stack out-params, lock, balanced refcount. Not read: the zero-count destroy virtual (object acquired
by this function, so within the stated allowance).

## 4. Game.GetFormFromFile [55465, A0C7E0] - READ

Read: 55465, 13731 (1B8180, walks the file list at dataHandler+0xD60 calling the `_stricmp` import), 13964
(1C7040, one compare), 14003 (1C8D70, writes only `[rdx]` = the callback's own home slot rsp+0x448), 14617
(1E01A0, read lock on global 400517, hash lookup in map 400507), 68220, 68233, 68239.
Error path: 12920 (18A900 = `__stdio_common_vsprintf_s` into the 0x400-byte stack buffer, 11046 returns a
pointer) with string "File \"%s\" does not exist or is not currently loaded." then `call [vm_vtable+0x10]`.

## 5. ObjectReference.GetBaseObject [56168, A2D060] - READ, caveat A

Read: 56168, 11810 (1640E0: 12329 with type 0x2D, returns extra+0x10), 12329, 68233, 68239.
Falls back to `[ref+0x40]`. Only non-stack writes: the read lock and the thread-local cache in 12329.

## 6. ObjectReference.GetDistance [56170, A2D0A0] - READ, caveat A on a rare path

Read: 56170, 19823 (2EDB90), 19494 (2D6E90), 20490 (307A70), TESObjectREFR slot +0x4B8 for ALL 15 vtables =
1 target 19817 (2ED8B0) -> tail jump 11728 (15F8C0: 12329 with type 0x0B) -> 12329.
19823 reads flags and positions (+0x54..+0x5C) and returns the distance; 19494 compares parent cells /
worldspaces. Slot +0x4B8 is only called when a reference has no parent cell; it reaches 12329 only when
`flags & 0x4400` is set. None path: `call [vm_vtable+0x10]` with "Cannot calculate distance between and
object and None".

## 7. ObjectReference.GetScale [56633, A44B70] - READ

Read: 56633, 19664 (2E09C0), 24763 (3BF500), 24374 (3A8DF0). 50 instructions total, no non-stack write, no
lock, no indirect call.

## 8. ObjectReference.Is3DLoaded [56188, A2D550] - READ

Slot +0x380 for ALL 15 TESObjectREFR-derived vtables = 2 targets:
- 19735 (2E5770): TESObjectREFR, Actor, Character, Explosion, ChainExplosion, Hazard, the 8 projectile
  classes. Reads TLS (+0x5F8 compared with `this`, then +0x5F0) or `[[ref+0x68]+0x68]`. No write.
- 40462 (734F40): PlayerCharacter. Calls slot +0x388 (PlayerCharacter = 41023, 758420: reads byte +0xBE3),
  reads player+0x8F8, or tail-jumps to 19735. No write.
Note the result of 19735 depends on a thread-local override (read only).

## 9. ObjectReference.IsInDialogueWithPlayer [56194, A2D7A0] - WRITES (conditional)

Read: 56194, 37199 (6603E0), 39347 (6E25D0), 19846 (2EFAC0), 12326 (179050), 12180 (1767C0), 68234 (CC9140,
write-lock acquire), 68240 (CC9390, write-lock release).
- self form type 0x3E (actor): 37199 -> 39347 reads `[[actor+0xF8]+0x10]+0x458`. 22 instructions, READ.
- otherwise, only if the base object's form type is 0x19: compares `[singleton 401099 +0x68]` with the
  reference's handle obtained from 19846 -> 12326. If the reference already has a handle (bit 10 of
  ref+0x28) only the caller's stack slot is written. **If it has none (and its refcount is non-zero)**,
  12326 allocates one under write lock 400621:
  - 17913E `mov [table 400622 + idx*16], ecx` (entry age + in-use bit)
  - 17915D call 12180: stores a counted pointer to the reference in the table entry (`lock inc [ref+0x28]`,
    not released - the table keeps it)
  - 179168 / 179176 `and/or dword ptr [ref+0x28]` (handle index + has-handle bit on the game object)
  - 179196 / 1791A0 / 1791AC free-list head/tail globals 400619 / 400620
  Idempotent after the first call for a given reference (the handle then exists), but it is persistent
  state on the object and in a global table, so strictly WRITES. Other verdicts are unaffected.
- other form types return false with no call.

## Coverage statement for virtual slots

Every virtual slot above was resolved for all classes that have RTTI vtables in SkyrimSE.exe: BGSKeywordForm
(24 vtables), TESForm (147), TESObjectREFR (15), BSExtraData (169). Not covered: vtables supplied by other
modules (SKSE plugins that subclass or patch vtables), and the `[vtbl+8]` destroy call taken when a
reference's count reaches zero inside 12332 / 12051 / 55287 / 12180.

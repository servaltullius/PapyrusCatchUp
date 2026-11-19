# Batch 4 review: NetImmerse.HasNode / GetNodeScale, ObjectReference.GetDisplayName, FormList.Find

AE 1.6.1170, unpacked exe, static reading only. RVAs are image-relative. "Read" = every reachable
instruction disassembled and read. Names come from CommonLibSSE-NG / RTTI / SKSE 2.2.8 headers and are
orientation only. Raw dumps left in this directory: `b4_gobn.txt` (GetObjectByName targets),
`b4_fbt_create.txt` + `b4_fbt2.txt` (flattened-bone-tree creation, AttachChild), `b4_refname.txt`,
`b4_refname2.txt`, `b4_refname3.txt` (GetReferenceName tree).

Summary: items 1 and 2 are **WRITES** (both are lazy-create getters). Item 3 is **READ**.

---

## 1. NetImmerse.HasNode / NetImmerse.GetNodeScale - WRITES (conditional, lazy node creation)

### 1a. What SKSE's ResolveNode calls (PapyrusNetImmerse.cpp:19, v2.2.8, built for 1.6.1170)

| Source expression | Engine side | Status |
|---|---|---|
| `obj->GetNiNode()` | TESObjectREFR virtual slot 0x70 = vtable +0x380 (GameReferences.h:187; CommonLib `Get3D2` "// 70") | covered by batch 3: 2 targets over all 15 vtables, ID 19735 and ID 40462 (-> slot +0x388 ID 41023), no write |
| `DYNAMIC_CAST(obj, TESObjectREFR, PlayerCharacter)` | GameRTTI.cpp: RVA 153CF2A = ID 109689 = `jmp [IAT 174FA18]` = VCRUNTIME140!`__RTDynamicCast` | thunk read; the CRT function itself lives in another DLL and was not read (pure RTTI walk by contract) |
| `player->loadedState` / `->node` | plain field reads: ref+0x68, then +0x68 | no engine call |
| `player->firstPersonSkeleton` | plain field read: player+0x8F0 (GameReferences.h:775) | no engine call |
| `result->GetObjectByName(&nodeName.data)` | NiAVObject virtual, see 1b | **this is where the write is** |

`GetNiRootNode` (slot 0x6F) is NOT used. No other slot or engine function is involved. HasNode returns
`object != NULL`; GetNodeScale reads `object+0x78` (local scale). So everything except the name lookup is
covered and read-only.

### 1b. NiAVObject::GetObjectByName - slot 0x2A, vtable +0x150

Slot index: SKSE NiObjects.h counted by hand (NiRefObject 0-1, NiObject 2-0x24, NiAVObject 0x25
UpdateControllers ... 0x29 Unk_25, 0x2A GetObjectByName); CommonLib NiAVObject.h says `// 2A`. Both agree.

Enumeration (`b3_rtti.py`, RTTI complete-object locators): **61 vtables, 61 classes, 3 distinct targets**.
Cross-checks: (1) all 61 RTTI classes that have NiAVObject among their bases have a matched vtable;
(2) a raw scan of the image for 8-byte pointers to the three functions finds exactly 33 + 27 + 1 = 61, all at
`matched_vtable + 0x150`. Not covered: vtables from other modules.

| Target | Classes | Code |
|---|---|---|
| **ID 70257, D1C080** (5 insns) | x33: NiAVObject, NiCamera, NiLight, NiAmbientLight, NiDirectionalLight, NiPointLight, NiSpotLight, NiGeometry, NiTriBasedGeom, NiTriShape, NiTriStrips, NiParticles, NiParticleSystem, NiMeshParticleSystem, NiParticleMeshes, BSGeometry, BSTriShape, BSDynamicTriShape, BSInstanceTriShape, BSMultiStreamInstanceTriShape, BSLODTriShape, BSLODMultiIndexTriShape, BSMeshLODTriShape, BSMultiIndexTriShape, BSSegmentedTriShape, BSSkinnedDecalTriShape, BSSubIndexTriShape, BSSubIndexLandTriShape, BSLines, BSDynamicLines, BSCubeMapCamera, BSParticleShaderGeometry, BSStripParticleSystem | `return this->name(+0x10) == *arg ? this : 0`. No call, no write. |
| **ID 70299, D1D9A0** (32 insns) | x27: NiNode, NiSwitchNode, NiBillboardNode, NiBSPNode, BSNiNode, BSFadeNode, BSLeafAnimNode, BSTreeNode, BSFaceGenNiNode, BSMultiBoundNode, BSMultiBoundRoom, BSOrderedNode, BSValueNode, BSRangeNode, BSBlastNode, BSDamageStage, BSDebrisNode, BSTempNode, BSTempNodeManager, BSClearZNode, BSPortalSharedNode, BSParticleSystemManager, BSMasterParticleSystem, BSSceneGraph, SceneGraph, ShadowSceneNode, BGSDecalNode | calls D1C080 on itself, then for i < word[this+0x122]: child = [[this+0x118]+i*8]; `call [child vtbl+0x150]` (D1D9E6). Only its own stack is written. No lock. |
| **ID 70743, D30380** (30 insns) | x1: **BSFlattenedBoneTree** | **creates nodes**, see below |

### The write: BSFlattenedBoneTree::GetObjectByName

Read: 70743 (D30380), 70734 (D30020), 68221 (CC8460, hash into caller stack), 70735 (D300D0) in full.

- D30397 `call D30020`: hashes the name, looks it up in the tree's bone-name map (tree+0x144 capacity,
  +0x150 sentinel, +0x160 entries) -> bone index, or -1 if the name is not a bone / index >= [tree+0x128].
  Read-only.
- D303A0..D303A8 `mov r8b,1 ; call D300D0(tree, index, create=true)`.
- D300D0: `bone = [tree+0x130] + index*0x80`. If `[bone+0x70]` (the bone's NiNode) is non-null it is returned
  (read-only). **If it is null** (and create is true, which this caller hard-codes):
  - D30128 recursive call for the parent bone (`word [bone+0x68]`; index < 0 returns the tree itself), so the
    whole missing parent chain is materialised too;
  - D3015E allocate 0x128 bytes (MemoryManager 68115), D30172 NiNode constructor (70287, vtable 19AB150);
  - D3018A `call [parent vtbl+0x1A8]` (= NiNode::AttachChild, ID 70289 D1D100 for both NiNode and
    BSFlattenedBoneTree vtables): writes the parent's child array (may reallocate it via 19377), child
    count, the new node's parent pointer/index (+0x30/+0x38), reference counts;
  - **D30190 `mov [bone+0x70], node`**, **D30194 `inc dword [tree+0x12C]`** (populated-bone counter);
  - D301A1 sets the node's name from bone+0x78 (70507 -> 69166, string-pool refcount);
  - D301A6..D30218 copy the bone's local transform (+0x00..+0x30) and world transform (+0x34..+0x64) into
    the node (+0x48.., +0x7C..).
- If nothing was found in the bone map, D303CA tail-jumps to the NiNode version (read-only).

CommonLib's BSFlattenedBoneTree.h agrees with the layout (BoneEntry 0x80 bytes, `node` at +0x70,
`numPopulatedBones` at +0x12C, bone map).

### Is a BSFlattenedBoneTree ever in a reference's 3D? (static evidence)

Vtable 19ADEC8 is referenced by 5 functions: 70755 (dtor, slot 0), 70733, 70745 (no direct caller found;
probably the stream creator), 70729 (D2F520 = CreateClone, slot +0xB8) and **70731 (D2F990)**, the builder.
70731 has exactly one caller: **273327 in ID 17667 (2726A0)**:

    273300 cmp byte [r15+0x1a], 0x2b        ; this->formType == 0x2B (NPC base form)
    273307 call 11437                       ; static BSFixedString table, [0] = "NPC Root [Root]"
    273315 call 76207 (root3D, &name, 1)    ; find that node
    273327 call 70731 (node)                ; if found and != root: flatten it

70731 requires the node's exact NiRTTI to be NiNode (410481), collects the NiNode subtree (70759), and if at
least one bone is droppable allocates a 0x168-byte BSFlattenedBoneTree, copies name/transforms per bone and
**detaches** (`[parent vtbl+0x1C0]`, D2FE6F) every bone that has no controller (+0x18), no collision object
(+0x40), no extra data (word +0x28) and no kept/non-NiNode descendant. Kept bones get `bone+0x70 = node`;
dropped bones get 0. So after an NPC model is built through 17667, the skeleton under "NPC Root [Root]" is a
BSFlattenedBoneTree with some bones that exist only as table entries until someone asks for them by name.
17667's callers are IDs 17133, 17236, 24721 (not read). Whether a given actor at run time really has
dropped bones was not measured - it is a static reachability statement. (The first-person skeleton pointer
is read from a field; whether its tree contains a flattened bone tree was not determined.)

### Verdict

**WRITES**, both natives (identical engine path).
- Condition: the searched 3D contains a BSFlattenedBoneTree, the name is in its bone map, and that bone has
  no NiNode yet (`[bone+0x70] == 0`), and the search reaches the tree before finding the name elsewhere.
- Effect: one or more NiNodes are allocated and attached into the live scene graph (D3018A), the bone table
  and the populated counter are updated (D30190, D30194).
- Idempotent per bone: the second call returns the stored node (D3010B..D30112). The node stays for the
  lifetime of the 3D. This is the same thing the stock call does later in the same frame, but it is a
  structural scene-graph mutation, not a cache.
- READ when the reference has no 3D, when the name is empty (root returned without lookup), when the name
  does not exist anywhere (D30020 returns -1), or when no flattened tree is in the searched 3D.
- Timing / threads: D1D9A0 walks child arrays and D300D0 / AttachChild mutates them (possible array
  reallocation in 19377) with no lock and no reference held on the root by SKSE. Safe only if no other
  thread walks or updates that skeleton at that moment. The stock call has the same exposure; whether the
  extra VM cycle is at an equivalent point of the frame was not verified here. The 3D getter 19735 also
  honours a thread-local override (TLS+0x5F8), read-only.
- Not read: `__RTDynamicCast` (VCRUNTIME140), the allocator below 68115, NiNode ctor callees (70272, 70318),
  19377 / 70322 / 70266 below AttachChild. All of these are on the write path only, so they do not change
  the verdict.

---

## 2. ObjectReference.GetDisplayName - WRITES (creates and attaches ExtraTextDisplayData; can create a handle)

SKSE body: `CALL_MEMBER_FN(object, GetReferenceName)()` = RVA 2EA340 = **ID 19781**
(CommonLib: `TESObjectREFR::GetDisplayFullName`, RELOCATION_ID(19354, 19781)).

Read in full: 19781 (2EA340, 156 insns), 11654 (15D900), 11719 (15F6F0), 16039 (2259D0), 11717 (15F690),
16074 (22E8C0), 11582 (151E50), 11703 (15F330), 12315 (178AE0), 11575 (151830), 14720 (1E2DA0), 12768
(184100), 12776 (1847B0), TESFullName slot +0x28 for ALL 49 vtables = 2 targets: 10880 (141190 -> 15452
2078B0, x48) and 25529 (3E7350, TESTopic), both read-only. Already known from batch 3: 12329, 12332, 12326,
68233/68239/68234/68240. Summarised only (b3_walk, not read line by line): 12775 (184500, the name builder)
and its callees, 69175 (CECA70, BSFixedString set), the allocator.

Flow of 19781 (`this` = r15, extra list = this+0x70):
1. 2EA366 `rsi = 11654(list)`: extra type 0x1C (reference handle) -> 12332 -> if that reference has form flag
   0x20, type 0x99 (ExtraTextDisplayData) from ITS list, else type 0x99 from the own list. Read-only
   (12329 TLS cache, balanced refcount).
2. If rsi == 0 (2EA37A..2EA443): own type-0x1C handle -> 12332 -> that reference's ExtraContainerChanges
   (type 0x15, 11717) -> 16074 finds the inventory entry whose object == this->baseObject (+0x40). If found:
   `test [this+0x28], 0x3FF ; jne` -> **2EA3FB `call 12326`** = get-OR-CREATE the reference's own handle
   (batch 3: allocates a slot in handle table 400622 under write lock 400621 and sets the handle bits in
   ref+0x28 when the reference has none). Then walks the entry's extra lists comparing their type-0x1C
   handle with it and takes that list's text data.
3. 2EA46E..2EA4B0: read lock on list+0x18, 11582 = presence bit of type 0x25 (ExtraHealth), unlock; if present
   `xmm6 = 11703()` (health), else 1.0.
4. If rsi == 0:
   - health within (0.999, 1.001) (constants 1AD2870 = 1.0, 178C6DC = 1.001, 178C6D8 = 0.999):
     2EA568 `14720(baseObject)` -> `__RTDynamicCast` to TESFullName -> `jmp [vtbl+0x28]`. Read-only.
   - **otherwise (2EA4E3..2EA55E): allocate 0x38 bytes, construct an ExtraTextDisplayData in place (vtable
     1771390 = ID 186855, RTTI `.?AVExtraTextDisplayData@@`; fields +0x10..+0x20 = 0, +0x28 = -1,
     +0x2C = 1.0, +0x30 = 0) and `call 12315(list, extra)` = ExtraDataList::Add**: write lock on list+0x18,
     11575 appends the node to the list ([list+8] / last->next at 15187C / 151882), allocates the 0x17-byte
     presence bitfield if missing (1518CB) and sets bit 0x99 (1518EF).
5. rsi != 0 (found or just created): 2EA57D `12768(extra, baseObject, health)` =
   ExtraTextDisplayData::GetDisplayName. Fast path (184127..184139): name non-empty and
   `[extra+0x2C] == health` -> return the cached string, read-only. Otherwise it **rebuilds and stores the
   name**: 12776 (custom-name case: +0x18 == 0, +0x20 == 0, +0x28 == -2; may truncate the stored name and
   write +0x2C at 184824), 12775 builds the text, **184176 `movss [extra+0x2C], health`**, **184195
   `69175(&extra->name(+0x10), text)`**.

This is the engine original of SKSE's `BaseExtraList::GetDisplayName` re-implementation: same create-and-add
when there is no text data and health != 1.0.

### Verdict

**WRITES.**
- W1 (2EA510 alloc, 2EA55E Add): reference has ExtraHealth outside (0.999, 1.001) and no ExtraTextDisplayData
  was found -> a new extra data object is attached to the reference's extra list (persistent game state,
  goes into the save). Idempotent afterwards (next call finds it).
- W2 (184176, 184195, 184824): an ExtraTextDisplayData exists but its cached name is empty or was cached for
  a different health value -> name string and cached factor rewritten on the extra data. Idempotent until
  health changes again.
- W3 (2EA3FB -> 12326): no text data found in step 1, the own type-0x1C handle resolves to a reference whose
  container changes hold an entry for this base object, and this reference has no handle yet -> handle
  created (global handle table + bits in ref+0x28). Idempotent afterwards.
- Pure read only when: no text data and health ~ 1.0 (or no ExtraHealth) and W3's precondition is false; or
  text data present with a non-empty name cached for the current health. Cannot be told from outside
  without repeating these checks.
- Other: 12329's thread-local cache (batch 3 caveat A). 11575 does not touch the extra-data epoch global
  400305 (no rip-relative write in it), so whether the TLS cache can hold a stale "no 0x99" entry for this
  list after W1 was not analysed.
- Not read: 12775's subtree and 69175 (write path only), the allocator, `__RTDynamicCast`, the zero-count
  destroy virtuals `[vtbl+8]` at 2EA462, 15D9AC, 22E9BB, 22E9E4.

---

## 3. FormList.Find - READ

Registration in ID 55671: A16CE5 `lea r8,[17B8580 "FormList"]`, A16CEC `lea rdx,[184E8C8 "Find"]`, A16CF6
constructor 104653, A16D07 `lea rax,[A16B00]`, A16D0E `mov [r12+0x50], rax`. Callback = **RVA A16B00 =
ID 55669** (only reference to A16B00 in the image is that lea). `natives.py` prints "callback not
recognised" for it because the store uses r12 (SIB byte: 49 89 44 24 50); the same is true for HasForm.

Read: 55669 (17 insns), 20906 (319AE0, 63 insns - the same function HasForm uses), 53824 (9B7E10, 82 insns,
error path only).
- form argument (r9) non-null: `mov rdx,r9 ; mov rcx,r8 ; jmp 20906`. 20906 scans the script-added form-ID
  array (list+0x38 -> data, count at +0x10) against form+0x14, then the form-pointer array (list+0x20,
  count +0x30), returns the index or -1. No call, no indirect call, no lock, the only memory writes are its
  four saved registers in the caller's home space.
- form argument None: A16B33 `call 53824(self, "Cannot find a None form", vm, stackId, severity 2)`, returns
  -1. 53824 saves and restores the thread-local memory-context tag (TLS+0x768 at 9B7E5C / 9B7F03), asks the
  VM's handle policy for the object's description (`[vtbl+0x168]`, `[+0x20]`, `[+0x58]`) and logs through
  53914 -> `[vm vtbl+0x10]`. It also contains a one-time function-local static BSFixedString (guard 406764,
  9B7F1F..9B7F5F) - a lazily initialised constant, first error only.

Verdict: **READ** (allowed writes only: stack, TLS memory tag save/restore, VM error log line on None).
Not read: below 53914 / the VM log virtual, and the handle-policy virtuals in 53824 (error path only).
Timing: reads both FormList arrays with no lock, same as HasForm; a concurrent AddForm/Revert on another
thread could reallocate them (same exposure as the stock call).

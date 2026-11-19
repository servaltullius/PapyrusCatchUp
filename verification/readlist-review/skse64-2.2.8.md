# SKSE64 native read/write review (batch of 18)

## Source actually read

- Repository: github.com/ianpatt/skse64, fetched verbatim with curl (raw.githubusercontent.com).
- **Tag `v2.2.8`** = commit `b9bfa27de9fe927daade322c5209180176604495`. Its `skse64_common/skse_version.h` says
  `SKSE_VERSION 2.2.8`, `SKSE_VERSION_RELEASEIDX 73`, `CURRENT_RELEASE_RUNTIME RUNTIME_VERSION_1_6_1170`.
  Copies: `<SCRATCH>\skse_228\`.
- **master** = commit `25b72352adb6543fa6d0bd3795780672b2e238e0` (2026-09-06). Its skse_version.h says
  **2.3.1**, RELEASEIDX 75, `CURRENT_RELEASE_RUNTIME RUNTIME_VERSION_1_7_104`. Copies: `<SCRATCH>\skse_src\`.
- Installed DLL `G:\TAKEALOOK\mods\Skyrim Script Extender (SKSE64)\Root\skse64_1_6_1170.dll` has
  FileVersion `0, 2, 2, 8` (read from the PE version resource; the DLL was not disassembled).

**master does NOT match the installed version** (master = 2.3.1 for runtime 1.7.104). The tag v2.2.8 does.
Diff v2.2.8 vs master for the files used here:
- All 11 `Papyrus*.cpp` files of this batch (Game, Form, Keyword, NetImmerse, ObjectReference, Actor, Armor,
  Spell, MagicEffect, Shout, SKSE) plus PapyrusArgs.cpp, GameBSExtraData.cpp, GameTypes.cpp: **byte-identical
  (ignoring CRLF)**.
- Game*.h / Game*.cpp / Ni*.h / PapyrusVM.h / PapyrusValue.h differ only in relocation addresses, struct
  offsets (PlayerCharacter +8, SkyrimVM +0x10) and an added NiAVObject flag enum. No logic change in any
  helper used by this batch.
- **All addresses below are from the v2.2.8 headers, i.e. RVA offsets from the image base of
  SkyrimSE.exe 1.6.1170.** The master headers carry different numbers (1.7.104) and must not be used.

Not verified: that the shipped DLL was actually built from tag v2.2.8 unmodified (version resource only).
`common/ICriticalSection.h` came from github.com/ianpatt/common master (untagged).

## Wrapper effects common to every SKSE native (PapyrusNativeFunctionDef_Base.inl `Run`, PapyrusArgs.cpp)

Not counted in the per-native verdicts, but they are engine code not read and they run on every call:

- `VMArgList::GetOffset` 0x014327E0, `VMArgList::Get` 0x01432850 (argument fetch).
- Non-static base / form argument: `UnpackHandle` -> `IObjectHandlePolicy::IsType`, `Unk_02`, `Resolve`
  (virtuals on `g_objectHandlePolicy`).
- `BSFixedString` argument: `StringCache::Ref::Set` 0x00CEC760 into a local that has **no destructor**
  (SKSE's `StringCache::Ref` never calls Release) -> one string-pool reference is leaked per call.
- Result int/float/bool: `VMValue::Destroy` 0x014259F0 then plain store.
- Result string: `VMValue::SetString` -> `VMValue::Destroy` 0x014259F0 + `StringCache::Ref::Set` 0x00CEC760.
- **Result Form/object: `PackHandle`** -> `VMClassRegistry::GetFormTypeClass`, `IObjectHandlePolicy::Create`,
  `VMClassRegistry::Unk_1C` (find existing identifier); if none: `VMClassRegistry::Unk_15` (creates a VM
  object identifier) and `BindID` -> `ObjectBindPolicy::BindObject` 0x0141DB30. Then `VMValue::Set` 0x01425AF0.
  So returning a form that has no script object yet **creates and binds a VM object** (VM-side state, not
  form data). Same mechanism the game's own natives use; flagged because it is a write to VM tables.

## Per native

### 1. Game.GetLightModName
- PapyrusGame.cpp:227 `papyrusGame::GetLightModName(StaticFunctionTag*, UInt32 index)`
- Reg (812): `new NativeFunction1 <StaticFunctionTag, BSFixedString, UInt32>("GetLightModName", "Game", ...)`
- NoWait: **no** (no SetFunctionFlags line for it; only the GetMod* non-light set is flagged).
- Body: `DataHandler::GetSingleton()` (`*g_dataHandler`, data ptr 0x020F6320), bounds check on
  `modList.loadedCCMods.count`, `tArray::GetNthItem` (inline copy), `return (modInfo) ? modInfo->name : NULL;`
- Engine: `StringCache::Ref::ctor(const char*)` 0x00CEC5D0 (implicit BSFixedString from `char name[0x104]`,
  `""` or NULL). Interns the string in the global string pool (pool lock; adds an entry if the file name is
  not already pooled; the reference is never released).
- Verdict: **READ-EXCEPT-ENGINE** (string pool only; no game object touched).

### 2. Game.GetModName
- PapyrusGame.cpp:115. Reg (793): `NativeFunction1 <StaticFunctionTag, BSFixedString, UInt32>("GetModName", "Game", ...)`
- NoWait: **yes** (line 932).
- Body: index > 0xFF -> `loadedCCMods` (index - 0x100), else `loadedMods`; bounds check; GetNthItem;
  `return (modInfo) ? modInfo->name : "";`
- Engine: `StringCache::Ref::ctor` 0x00CEC5D0. Verdict: **READ-EXCEPT-ENGINE**.

### 3. Game.GetModByName
- PapyrusGame.cpp:102. Reg (790): `NativeFunction1 <StaticFunctionTag, UInt32, BSFixedString>("GetModByName", "Game", ...)`
- NoWait: **yes** (line 931).
- Body: `pDataHandler->LookupModByName(name.data)` -> GameData.cpp:78
  `modList.modInfoList.Find(LoadedModFinder(modName))` -> `_stricmp(modInfo->name, m_stringToFind)` over the
  tList (inline, read-only); then `IsActive()` (`modIndex != 0xFF`), `IsLight()` (`fileFlags & 0x200`), returns
  `lightIndex + 0x100` or `modIndex`.
- Engine calls in body: none. Verdict: **READ**. Cost: linear case-insensitive scan of every plugin file
  known to the DataHandler (installed, not only active).

### 4. Form.GetName
- PapyrusForm.cpp:21. Reg (464): `NativeFunction0 <TESForm, BSFixedString> ("GetName", "Form", ...)`
- NoWait: **no** (the 20 flagged Form functions, lines 577-596, do not include GetName).
- Body: `DYNAMIC_CAST(thisForm, TESForm, TESFullName)`; `return pFullName->name.data;` else NULL.
- Engine: `__RTDynamicCast` 0x0153CF2A (via `Runtime_DynamicCast`, RTTI_TESForm 0x02038168 ->
  RTTI_TESFullName 0x02038140); `StringCache::Ref::ctor` 0x00CEC5D0.
- Note: reads the `TESFullName::name` field directly; it does NOT call the virtual `TESFullName::GetName` and
  does not touch extra data. Verdict: **READ-EXCEPT-ENGINE**.

### 5. Keyword.GetKeyword
- PapyrusKeyword.cpp:18. Reg (62): `NativeFunction1 <StaticFunctionTag, BGSKeyword*, BSFixedString> ("GetKeyword", "Keyword", ...)`
- NoWait: **no** (file has no SetFunctionFlags).
- Body:
  ```
  s_keywordCacheLock.Enter();
  if (s_keywordCache.empty()) { ... for each DataHandler->keywords:
      s_keywordCache.insert(KeywordCache::value_type(BSFixedString(pKeyword->keyword.Get()), pKeyword)); }
  s_keywordCacheLock.Leave();
  KeywordCache::iterator it = s_keywordCache.find(keyword);
  ```
- **WRITES**: fills the SKSE-private static `std::map<BSFixedString, BGSKeyword*> s_keywordCache` whenever it
  is empty (first call; every call if the keyword array is empty at that time). No game object or game global
  is modified; the cache is never invalidated (the invalidate code is commented out), so keywords created
  later are not found.
- Engine: `StringCache::Ref::ctor` 0x00CEC5D0 once per keyword during the fill.
- Other: Win32 CRITICAL_SECTION (`ICriticalSection`) on every call; the `find` runs after `Leave()`
  (unlocked). First call is O(N log N) over all keywords plus heap allocations - real time.

### 6. NetImmerse.HasNode / 7. NetImmerse.GetNodeScale
- PapyrusNetImmerse.cpp:38 `HasNode`, :153 `GetNodeScale`, both via :19 `ResolveNode`.
- Reg (384): `NativeFunction3<StaticFunctionTag, bool, TESObjectREFR*, BSFixedString, bool>("HasNode", "NetImmerse", ...)`
- Reg (411): `NativeFunction3<StaticFunctionTag, float, TESObjectREFR*, BSFixedString, bool>("GetNodeScale", "NetImmerse", ...)`
- NoWait: **no** for both - the SetFunctionFlags lines (462-467) are inside a `/* ... */` comment.
- ResolveNode: `obj->GetNiNode()`; `DYNAMIC_CAST(obj, TESObjectREFR, PlayerCharacter)`; for the player
  `firstPerson ? player->firstPersonSkeleton : player->loadedState->node`;
  `result->GetObjectByName(&nodeName.data)`. HasNode returns `object != NULL`; GetNodeScale returns
  `object->m_localTransform.scale`.
- Engine (not read): `TESObjectREFR::GetNiNode` = virtual, vtable slot 0x70 (Get3D; Actor/PlayerCharacter
  overrides); `NiAVObject::GetObjectByName` = virtual, slot 0x2A (index derived from header order and the
  "SE: function 29/30" comments, not stated explicitly; NiNode override recurses children);
  `__RTDynamicCast` 0x0153CF2A.
- Verdict: **READ-EXCEPT-ENGINE**. Other: walks the live scene graph with no lock and no NiPointer held;
  cost is a recursive name search over the skeleton.

### 8. ObjectReference.GetDisplayName
- PapyrusObjectReference.cpp:476. Reg (629): `NativeFunction0<TESObjectREFR, BSFixedString>("GetDisplayName", "ObjectReference", ...)`
- NoWait: **no** (file has no SetFunctionFlags).
- Body (whole): `return (object) ? CALL_MEMBER_FN(object, GetReferenceName)() : "";`
- Engine (not read): **`TESObjectREFR::GetReferenceName` 0x002EA340**; `StringCache::Ref::ctor` 0x00CEC5D0.
- It does NOT call SKSE's `BaseExtraList::GetDisplayName` (GameBSExtraData.cpp:252, the one that does
  `xText = ExtraTextDisplayData::Create(); Add(kExtraData_TextDisplayData, xText);` when there is no text
  data and health != 1.0). Whether the engine function at 0x002EA340 does the equivalent internally is
  exactly the open question - SKSE's helper is a re-implementation of engine behaviour, so this is the
  highest-risk item of the batch. Unverified here.
- Verdict: **READ-EXCEPT-ENGINE** (SKSE side is a pure pass-through; treat as unproven until 0x002EA340 is read).

### 9. Actor.GetWornForm
- PapyrusActor.cpp:131. Reg (640): `NativeFunction1 <Actor, TESForm*, UInt32>("GetWornForm", "Actor", ...)`
- NoWait: **no** (file has no SetFunctionFlags).
- Body: `thisActor->extraData.GetByType(kExtraData_ContainerChanges)` (GameBSExtraData.cpp:190: BSReadLocker,
  `HasType` presence-bit test, list walk with `Runtime_DynamicCast`); `pContainerChanges->FindEquipped(matcher)`
  (GameExtraData.cpp:202: `data->objList->Visit(GetMatchingEquipped)`; per entry `MatchBySlot::Matches` =
  `DYNAMIC_CAST(pForm, TESForm, BGSBipedObjectForm)` and `(pBip->data.parts & m_mask) != 0`; then
  `pExtraDataList->HasType(kExtraData_Worn / WornLeft)` under BSReadLocker); returns `eqD.pForm`.
- Engine (not read): `BSReadWriteLock::LockForRead` 0x00CC90C0, `UnlockRead` 0x00CC9380 (modifies only the
  lock word of the extra list, repeatedly and nested); `__RTDynamicCast` 0x0153CF2A.
- Verdict: **READ-EXCEPT-ENGINE**. Notes: the inventory entry list itself is walked without the inventory
  lock; `FoundEquipData equipData;` is uninitialised when `data`/`objList` is null, so a garbage pointer can
  be returned (bug, not a write). Cost: linear over inventory entries.

### 10. Actor.GetEquippedObject
- PapyrusActor.cpp:154. Reg (643): `NativeFunction1 <Actor, TESForm*, UInt32>("GetEquippedObject", "Actor", ...)`
- NoWait: **no**.
- Body: slot 2 -> `thisActor->equippedShout`; else `Actor::GetEquippedObject(slot == 0)` (GameReferences.cpp:65:
  `processManager->equippedObject[Left|Right]`, NULL if no processManager).
- Engine calls in body: none. Verdict: **READ**.

### 11. Armor.GetMaskForSlot
- PapyrusArmor.cpp:52. Reg (166): `NativeFunction1 <StaticFunctionTag, UInt32, UInt32> ("GetMaskForSlot", "Armor", ...)`
- NoWait: **no** (file has no SetFunctionFlags).
- Body: `if (slot < 29 || slot > 61) return 0; return (1 << (slot - 30));` Pure arithmetic. Verdict: **READ**.

### 12. Spell.GetNthEffectMagicEffect
- PapyrusSpell.cpp:30 -> `magicItemUtils::GetNthEffectMagicEffect` (:115).
- Reg (247): `NativeFunction1<SpellItem, EffectSetting*, UInt32>("GetNthEffectMagicEffect", "Spell", ...)`
- NoWait: **yes** (line 294).
- Body: `thisMagic->effectItemList.GetNthItem(index, pEI); return (pEI) ? pEI->mgef : NULL;`
- Engine calls in body: none. Verdict: **READ**.

### 13-15. MagicEffect.GetCastingArt / GetHitEffectArt / GetLight
- PapyrusMagicEffect.cpp:169 / :180 / :114.
- Reg (372): `NativeFunction0<EffectSetting, BGSArtObject*>("GetCastingArt", "MagicEffect", ...)`
- Reg (379): `NativeFunction0<EffectSetting, BGSArtObject*>("GetHitEffectArt", "MagicEffect", ...)`
- Reg (337): `NativeFunction0<EffectSetting, TESObjectLIGH*>("GetLight", "MagicEffect", ...)`
- NoWait: **yes** for all three (lines 456, 458, 446).
- Body: `return (thisEffect) ? thisEffect->properties.castingArt|hitEffectArt|light : NULL;`
- Engine calls in body: none. Verdict: **READ** (x3).

### 16-17. Shout.GetNthSpell / GetNthWordOfPower
- PapyrusShout.cpp:10 / :6.
- Reg (48): `NativeFunction1<TESShout, SpellItem*, UInt32>("GetNthSpell", "Shout", ...)`
- Reg (45): `NativeFunction1<TESShout, TESWordOfPower*, UInt32>("GetNthWordOfPower", "Shout", ...)`
- NoWait: **yes** for both (lines 63, 62).
- Body: `return (thisShout && n < 3) ? thisShout->words[n].spell|word : NULL;`
- Engine calls in body: none. Verdict: **READ** (x2).
- (A separate `Actor.GetNthSpell` exists at PapyrusActor.cpp:649; not part of this batch.)

### 18. SKSE.GetVersionRelease
- PapyrusSKSE.cpp:23. Reg (54): `NativeFunction0<StaticFunctionTag, UInt32>("GetVersionRelease", "SKSE", ...)`
- NoWait: **yes** (line 62).
- Body: `return SKSE_VERSION_RELEASEIDX;` (73 in v2.2.8). Compile-time constant. Verdict: **READ**.

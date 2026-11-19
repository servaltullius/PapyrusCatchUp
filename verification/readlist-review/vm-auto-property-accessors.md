# Batch 5 review: Papyrus auto-property function objects (AE 1.6.1170, static reading only)

All RVAs are in the unpacked exe. Names come from CommonLibSSE-NG / RTTI and are orientation only.
Helpers added in this scratch directory: `b5_show.py` (print functions out of a b1_dump file), `b5_str.py`
(C string at RVA), `b5_slots.py` (bodies of IFunction slots of the four vtables). Dumps: `b5_get_tree.txt`
(b1_dump of 105248, depth 12), `b5_walk_success.txt` (b3_walk with allocator/CRT/error-path stops),
`b5_105302.txt`, `b5_105385.txt`, `b5_105428.txt`, `b5_104478.txt`.

## 1. AutoPropGetFunction::Call  ID 105248, RVA 146AB50 (133 insns, read completely)

Arguments: rcx = this, rdx = `BSTSmartPointer<Stack>*`, r8 = ErrorLogger*, r9 (VM) and the bool are unused.

Step by step
1. `frame = stack->top` (stack+0x60). Inlined TypeInfo decoding of `frame->self.varType` (frame+0x28):
   error unless the raw type is "object, not array" (146ABC3), not None (146AC00) and
   `frame->self.value != 0` (146AC46 `cmp qword [frame+0x30],0`).
   No check of the Object's valid/initialized bits, no bounds check of the variable index, **no lock taken**.
2. 146AC51 call 104287 (1424770, Variable -> `BSTSmartPointer<Object>&`; returns `&self.value`, no copy, so
   the owner object's refcount is NOT touched).
3. `src = obj + (this->[0x28] + 3) * 16` = `&obj->variables[varIndex]` (Object::variables at +0x30),
   `dst = &stack->returnValue` (stack+0x70); 146AC6B call 104297 (1425AF0, Variable copy-assign).
4. Saves the TLS memory tag (gs:[0x58] -> block[tls_index 415542] + 0x768), sets 0x14, restores (146AC97 /
   146AD06). Returns eax = 0 (kCompleted) on every path.

The value is delivered into **stack->returnValue** (type at stack+0x70, value at stack+0x78), not into a
frame variable.

Error path (self not an object / null), from 146ACA2: 54424 (9D9EE0, static None Variable, own static
guard 406758) -> 104297 sets returnValue = None; 12920 formats "Missing object for getting property" (string 252798,
1AA3AE0); 104444 (142EB00) appends the stack trace into a 0x400 stack buffer; 104432 (142DD80,
ErrorLogger, severity 2) bumps the per-thread error counter map at logger+0x70 under the spin lock at
logger+0x60, calls logger vtbl+8 and dispatches the log event to the sinks at logger+8 (104457).

### Variable copy-assign 104297 (1425AF0), read completely
- TLS tag save/set/restore (1425B31 / 1425D26).
- 1425B38 call 104296 (14259F0) = destroy the OLD content of dst (the previous return value):
  string -> 69164/69192 (pool entry refcount -1, entry removed from the pool and freed at zero);
  object -> 23255 -> 104253 Object::DecRef (and at zero 104246 Object destructor + 68117 free);
  array -> 53605 (`lock xadd [arr],-1`, at zero 104462 element destructor + free); then
  1425AC6 `mov qword [dst+8],0`.
- by source type:
  - int / float / bool / None: 1425C81 `mov [dst+8], [src+8]`.
  - string: 104285 (14245F0) -> 69162 (CEC680) copy to a stack temp, copy to dst+8, 69164 release temp.
    Net: pool entry refcount +1 (`lock cmpxchg [entry-0x10]` at CEC6A6, saturating at 0x7FFF).
  - object: 104287, 104252 Object::IncRef (1422EA0) for a stack temp, 1425C1D `mov [dst+8],obj`,
    IncRef again, 104253 DecRef for the temp. Net: refcount at Object+0x28 +1.
  - array: `lock inc [arr]` x2 (1425CC7, 1425CD3), 1425CCA store, `lock xadd -1` (1425CDB). Net +1.
- 1425D1F `mov [dst], [src]` (type word).

### Every write that is not to the function's own stack frame (success path)
| # | Where | What | Condition | Class |
|---|---|---|---|---|
| W1 | 146AC97/146AD06, 1425B31/1425D26, 1425A1F/1425ACE | TLS memory tag set to 0x14 and restored | always | allowed |
| W2 | 1425AC6, 1425C1D / 1425C81 / 1425CCA / CEC6B1, 1425D1F | stack->returnValue value + type | always | delivery |
| W3 | 104296 -> CED9A0 (CEDB1A/CEDB61), 1422F9D/1422FDB, 9A882C | release of the PREVIOUS return value; at zero: string pool removal, Object destructor 104246 (`and [obj],~3` 14221D8, `lock dec [lockStructure]` 14221E5, type-info release), array destructor, 68117 free | previous returnValue was string/object/array | consequence of replacing the return slot (same in every function that returns a value) |
| W4 | CEC6A6; 1422F25 (x2) + 1422FDB; 1425CC7/1425CD3/1425CDB | refcount +1 on the delivered string entry / Object (+0x28) / Array (+0) - the reference now owned by returnValue | value is string/object/array | delivery (not balanced inside Call by design) |
| W5 | 1422EED `lock cmpxchg [obj+0x28],0x80000001`, 1422F05 `call [policy vtbl+0x48]`, 1422F0B `mov [obj+0x28],2` | Object::IncRef 1 -> 2 transition calls IObjectHandlePolicy::PersistHandle(obj->handle). Policy global 414391 (3329D50) is written only by the Object constructor 104255 (14230C2); the only implementation in the exe is SkyrimScript::HandlePolicy slot 9 = 53513 (9A2230): if HandleIsType(0x3D, handle) -> spin lock policy+8, count map at policy+0x18 incremented/inserted (53531, 9A2391), bool map at policy+0x48 updated (53549 / 53532), unlock | property value is an Object whose refcount is exactly 1 when read (i.e. the property variable is its only holder) AND its handle is a TESObjectREFR handle | **outside the allow-list**: transient keep-alive bookkeeping in the handle policy; undone by ReleaseHandle (53514) when the copy is dropped (2 -> 1) |
| W5' | 1422F9D, 1422FB5 `call [policy vtbl+0x50]` | ReleaseHandle on the previous return value's Object when its count goes 2 -> 1 | previous returnValue was such an Object | same class as W3 |
| W6 | 14247E4..142480D (guard 414401), 1425D37..1425D6E (guard 414403, 1425D50 writes static 414402), 142466B..14246A8 (guard 414399, static "" 414398) | MSVC function-local static initialisation (`_Init_thread_header` 109616, atexit 109608, footer 109615) | only the first time in the process that the static is touched | **outside the allow-list**, once per process, not game state; not provable statically that it already ran |

Not written by the getter: the owner script object (variables, flag word +0, lockStructure +0x18, handle,
refcount), the ObjectTypeInfo, the PropertyTypeInfo, the function object itself, the stack frame, any VM
table. No lock is left held (the only locks are the Object refcount spin bit and the policy spin lock, both
released on the same path).

Note on W5 frequency: CommonLib declares `AttachedScript : BSTPointerAndFlags<BSTSmartPointer<Object>,1>`
in `VirtualMachine::attachedScripts`, i.e. the VM table holds a counted reference. If so, an Object that is
still bound has count >= 2 as soon as a property variable refers to it and W5 never fires; count == 1 would
mean an orphaned Object kept alive only by that variable. This is an inference from names, NOT verified in
the machine code.

### Read / not read
Read by eye: 105248, 104287, 104297, 104296, 104285, 69162, 69164, 104252, 104253, 23255, 53605, 104462,
104246, 104317, 104299, 55114, 54424, 12920, 11046, 104432, 104447, 53513, 53514.
Read as write/call summary only (b3_walk): 69192 (CED9A0), 69200, 69201, 69161, 69197, 12340, 13129,
53531, 53532, 53549, 53547, 53548, 53505 (HandleIsType), 14617, 104457.
NOT read: engine allocator 68115 / 68117 / 68121 internals; CRT 109616 / 109608 / 109615 / 109588;
ObjectTypeInfo destructor 104323 (1427240, reached only when the previous return value held the last
reference to an Object that held the last reference to its type); stack-trace formatter 104444 / 104486
(error path); ErrorLogger virtual [vtbl+8] and the event sinks (error path); the three indirect calls
inside HandleIsType 53505 ([form vtbl+0x18], [+0x160]); whether returnValue is empty at entry.

## 2. Contrast

### AutoPropSetFunction::Call  ID 105268, RVA 146AEF0 (140 insns, read)
Same self checks. 146AFF6 104287 (object), 146B00A 104484 (1432850 -> 104494: address of frame variable 0 =
the argument; writes only caller stack out-params), then
`146B00F mov ecx,[this+0x28]; add rcx,3; shl rcx,4; add rcx,[rbx]` and **146B021 call 104297 with
dst = &obj->variables[varIndex], src = argument** - i.e. it destroys the old value of the script object's
variable and stores the new one (with the refcount / PersistHandle effects above applied to the object's
variable). Then always 146B038: returnValue = None. Error string 252799 "Missing object for setting
property". WRITES the script object.

### NativeFunctionStub::Call  ID 104715, RVA 1440D60 (58 insns, read)
Formats `error: Unbound native function "%s" called` (252570) with this->[0x10] (name), logs it through
104432 with severity 2, sets returnValue = None (static 406756 via 104297), returns 0. Never runs a native.

## 3. IFunction slots (vtable has 21 slots, 0x00..0x14; no GetIsLatent slot exists)

| slot | Getter (252795) | Setter (252797) | Stub (252808) | NativeFunctionBase (252545) | ScriptFunction (252888) |
|---|---|---|---|---|---|
| 04 GetReturnType | [this+0x20] | 0 (None) | [this+0x28] | | [this+0x28] |
| 05 GetParamCount | 0 | 1 | word [this+0x38] | | word [this+0x38] |
| 07 GetStackFrameSize | 0 | 1 | word [this+0x38] | | computed |
| 08 GetIsNative | 1 | 1 | 1 | 1 | **0** (`xor al,al`, 14793F0) |
| 09 GetIsStatic | 0 | 0 | byte [this+0x40] | | byte [this+0x5A] |
| 0A GetIsEmpty | 0 | 0 | 0 | | [this+0x48]==0 |
| 0B GetFunctionType | **1 (kGetter)** 146AB10 | **2 (kSetter)** 146AEB0 | 0 (1440D20) | 0 (143DAA0) | word [this+0x58] |
| 0C GetUserFlags | 0 | 0 | [this+0x44] | | [this+0x40] |
| 0E InsertLocals | ret | ret | ret | | real |
| 13 CanBeCalledFromTasklets | 1 (146AD80) | 1 (146B160) | byte [this+0x41] | byte [this+0x41] (143DBE0) | 1 |
| 14 SetCallableFromTasklets | ret | ret | stores +0x41 | | ret |

`GetIsNative() && GetFunctionType() == 1` is true only for AutoPropGetFunction among the function classes
in the exe (a user-written getter is a ScriptFunction: type 1 but native 0). IFunction subclasses defined
by other DLLs were not checked.

## 4. Identity and lifetime

- Only one code reference to the getter vtable: 146A823 in **ID 105232 (146A730)**; setter: 146A9A3 in
  ID 105233 (146A8B0). 105232(PropertyTypeInfo* prop, BSTSmartPointer<IFunction>* out):
  - `prop->getFunction` (prop+0x20) non-null -> `*out = prop->getFunction` (146A765, counted).
  - else if `prop->autoVarIndex` (prop+0x30) != -1 -> **allocates 0x30 bytes** (146A7F8 `edx=0x30`,
    68115) and constructs: +0x00 vtable, +0x08 refcount 0, +0x10 BSFixedString copy of prop+0x00
    (parentObjName), +0x18 copy of prop+0x08 (propertyName), +0x20 = prop+0x10 (TypeInfo), +0x28 =
    prop+0x30 (variable index, u32); stores it in `*out` only.
  - **Nothing is cached**: the new object is not written back to the PropertyTypeInfo or the type.
- Callers of 105232: 104853 AttemptFunctionCall +0x35C (144E6CC; `cmp [rsp+0x48],2` picks getter vs setter
  factory 105233 at 144E715; `prop` is a stack copy at rbp-0x30 filled by 104845), 104478 +0x31E (stack
  frame reconstruction, function type 1/2), 105428 (link-time signature check, only for
  autoVarIndex == -1, so it never builds an auto function).
- Destructor 105274 (146B180): releases the two strings, `operator delete(this, 0x30)`.
- Consequence: AutoPropGet/SetFunction objects are **transient, one fresh 0x30-byte allocation per
  property call**, alive only while the smart pointers in AttemptFunctionCall / the stack frame hold them.
  One property under many addresses is expected. The reverse also holds: a freed 0x30 block can be reused
  for a different property or for a SETTER object (same size, same allocator), so an address is not an
  identity. Identity is (vtable, +0x10 class name, +0x18 property name, +0x28 variable index).

## 5. Non-auto property with a hand-written Get

Property linker ID 105385 (1473F70), per unlinked property (r13: +0x00 permissions, +0x10 read handler,
+0x18 write handler, +0x20 auto variable name, +0x28 doc string, +0x30 user flags):
- autoVarIndex starts at -1 (1474178). Only if the auto variable name is not "" it is resolved to a
  variable index (147451A); errors 252875 / 252876 ("Auto Property ... uses variable ...").
- read handler present -> 147434F call 105302 -> function object in [rsp+0x78]; write handler -> 147444F.
- 14744B5..14744EA: `getFunction = [rsp+0x78]` only if permissions are 1 or 3 AND autoVarIndex == -1
  (setter symmetric at 14745BA..14745E2). Property stored with 105446 (1474619).
- 105302 (146D820): `cmp byte [func+0x10],0` (146D8AA) - not native -> ScriptFunction (0x80 bytes,
  constructor 105450 at 146DE50, vtable 252888, function type copied to +0x58). Native with function type
  != 0 (146D947) -> error 252840 "%s.%s() is flagged as a non-normal function. Native functions must be
  normal." and no object; native + normal -> NativeFunctionStub (0x50 bytes, 146DACF).
- Hence a non-auto property getter is always a ScriptFunction (GetIsNative == 0) and 105232 returns that
  object instead of synthesising an AutoPropGetFunction. CONFIRMED from the code.
  Not checked: other writers of PropertyTypeInfo+0x20 outside 105385 (e.g. a hot-reload path).

## Verdict for the getter

READ with respect to script and VM state: on every path the only thing it stores is the return value
(plus allowed items), and it never writes the script object, its type, or the property.
Strict-letter exceptions, both reported above: W5 (PersistHandle bookkeeping when the delivered Object's
refcount was exactly 1 - conditional, transient, also done by every other copy of an object value) and W6
(once-per-process static guards). Not read: allocator, CRT, ObjectTypeInfo destructor, error-path
formatter and logger virtuals (see list in section 1).

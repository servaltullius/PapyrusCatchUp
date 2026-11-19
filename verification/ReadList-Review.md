# 읽기 목록 검토 기록 (2026-11-10)

`accelerate` 모드가 읽기로 인정하는 네이티브 함수의 근거입니다. 대상은 Skyrim AE 1.6.1170 실행 파일과 SKSE64 2.2.8입니다. 목록 자체는 `src/NativeClassification.h`의 `reviewedReads`에 있고, 실행 중 판정은 `src/VMCatchUp.cpp`의 `IsReviewedRead`가 합니다.

## 대상을 고른 방법

Save93 장비 변경 실행의 호출 집계([Save93-Native-Census-2026-11-10.log](Save93-Native-Census-2026-11-10.log))에서 이름이 조회처럼 보이는 함수 중 가장 많이 불린 60종을 뽑고, 그중 게임 실행 파일(34개)과 SKSE(18개)가 등록한 52개를 검토했습니다. 다른 플러그인의 함수는 넣지 않았습니다.

## 판정 기준

| 판정 | 뜻 |
| --- | --- |
| 읽기 | 도달 가능한 코드를 모두 읽었고, 아래 "허용한 쓰기" 말고는 메모리에 쓰지 않음 |
| 쓰기 | 게임 객체나 전역에 데이터를 만들거나 붙이거나 캐시하는 경로가 하나라도 있음. 한 번만 일어나거나 결과가 같아도 쓰기로 봄 |
| 미확인 | 다 읽지 못함 |

허용한 쓰기는 다음과 같습니다. 스크립트나 게임이 다시 읽을 수 있는 것을 바꾸지 않는 것들입니다.

- 자기 스택 프레임, 호출자 스택을 가리키는 출력 인자
- 짝이 맞는 참조 수 증감, 잠금 획득과 해제
- 스레드 전용 저장 공간: 메모리 태그(`gs:[0x58]` 슬롯), 추가 데이터 조회(ID 12329)의 스레드별 캐시. 이 캐시는 전역 세대 번호(ID 400305, 읽기만 함)로 유효성을 확인하고, 목록·참조·전역에는 쓰지 않음
- 반환 문자열의 문자열 풀 등록
- 기본 객체 표의 함수 내 정적 1회 초기화(가드 ID 400264). 실행 파일 안에서 약 150곳이 같은 가드를 쓰며, 게임이 데이터를 불러오는 동안 이미 초기화된다고 보았음. 정적으로 증명한 것은 아님
- 인자가 None이거나 잘못됐을 때 VM이 남기는 오류 로그

## 게임 실행 파일의 함수 (처음 검토한 34개 중 읽기 28, 쓰기 6. 뒤에 읽기 2개 추가)

콜백은 등록 코드에서 뽑았습니다. 등록 코드는 함수 객체의 `+0x50`에 콜백을 저장합니다(ID 55394의 `mov [obj+0x50], rax`, 생성자 ID 54821·54822·54833의 같은 저장).

| 함수 | 콜백 ID | 판정 | 근거 |
| --- | --- | --- | --- |
| `Actor.GetRace` | 54930 | 읽기 | 본문이 `mov rax,[r8+0x1F8]; ret` |
| `Alias.GetOwningQuest` | 55155 | 읽기 | `mov rax,[r8+0x10]; ret` |
| `Game.GetPlayer` | 55469 | 읽기 | 전역 포인터 하나를 읽고 반환 |
| `GlobalVariable.GetValue` | 55930 | 읽기 | `movss xmm0,[r8+0x34]; ret` |
| `ObjectReference.GetPositionX/Y/Z` | 56178, 56179, 56180 | 읽기 | `movss xmm0,[r8+0x54/0x58/0x5C]; ret` |
| `ObjectReference.IsDisabled` | 56639 | 읽기 | 플래그 비트 하나 |
| `Actor.IsDead` | 54705 | 읽기 | 가상 슬롯 +0x4C8 = ID 37483 (Actor, Character, PlayerCharacter 공통), 필드 읽기뿐 |
| `Actor.IsInCombat` | 54945 | 읽기 | +0x718 = ID 38562 (Actor, Character), 41024 (PlayerCharacter) |
| `Actor.IsSneaking` | 54953 | 읽기 | ID 38986, 17965: 읽기 잠금 아래 비트 검사 |
| `Actor.HasPerk` | 54644 | 읽기 | ID 37698 → 40021, 스택 위 방문자 객체에만 씀 |
| `Actor.IsInFaction` | 54946 | 읽기 | +0x7C8 = ID 37687 (Actor, Character), 40679 (PlayerCharacter). 스레드별 캐시만 씀 |
| `Actor.GetFactionRank` | 54686 | 읽기 | ID 37676 → 12329, 12651, 14393. 스레드별 캐시만 씀 |
| `Actor.HasMagicEffect` | 54649 | 읽기 | ID 34517 → 38795 → 39871 → 34056 |
| `Actor.HasMagicEffectWithKeyword` | 54650 | 읽기 | ID 34518, 스택 위 함수 객체에만 씀 |
| `Actor.GetEquippedWeapon` | 54924 | 읽기 | ID 38574 → 39806, 필드 읽기뿐 |
| `Actor.GetEquippedArmorInSlot` | 54684 | 읽기 | 인벤토리 조회(ID 16039 → 11717 → 12329)는 없으면 만들지 않는 단순 조회 |
| `Actor.WornHasKeyword` | 54777 | 읽기 | 같은 단순 조회, 키워드 폼 슬롯 +0x28 |
| `ActorBase.GetSex` | 56005 | 읽기 | ID 24374, 플래그 비트 |
| `Form.HasKeyword` | 55870 | 읽기 | ID 14258과 가상 슬롯 네 개. 실행 파일의 TESForm 계열 vtable 147개, 키워드 폼 vtable 24개를 모두 열거 |
| `FormList.HasForm` | 55682 | 읽기 | 콜백과 ID 20906, 71개 명령, 쓰기·잠금·간접 호출 없음 |
| `ReferenceAlias.GetReference` | 55287 | 읽기 | 읽기 잠금, 해시 조회, 핸들을 참조로 바꾼 뒤 참조 수 반환 |
| `Game.GetFormFromFile` | 55465 | 읽기 | 파일 목록 비교, 폼 맵 읽기 잠금 |
| `ObjectReference.GetBaseObject` | 56168 | 읽기 | ID 11810 → 12329 |
| `ObjectReference.GetDistance` | 56170 | 읽기 | ID 19823, 19494, 20490 |
| `ObjectReference.GetScale` | 56633 | 읽기 | 함수 4개, 명령 50개 |
| `ObjectReference.Is3DLoaded` | 56188 | 읽기 | 가상 슬롯 +0x380, 참조 vtable 15개에서 대상 둘(ID 19735, PlayerCharacter 40462) |
| `Actor.GetActorValue` | 54675 | **쓰기** | ID 38462가 현재 값 캐시를 다시 채움(ID 40230), 38464로도 이어짐 |
| `Actor.GetActorValuePercentage` | 54677 | **쓰기** | ID 37337 → 38463이 최대값 캐시를 다시 채움(ID 40233) |
| `Actor.GetBaseActorValue` | 54678 | **쓰기** | ID 38464 → 39017: 저장된 기본값이 없으면 전역 쓰기 잠금(ID 404014) 아래 액터의 값 표에 항목 삽입 |
| `Actor.HasSpell` | 54653 | **쓰기** | 인자가 주문일 때 ID 38782 → 38781 → 39443이 레벨드 주문 목록을 만들어 프로세스 데이터에 저장. 인자가 외침이면 읽기 |
| `ObjectReference.GetItemCount` | 56173 | **쓰기** | 인자가 참조일 때 ID 19846 → 12326이 핸들을 새로 만듦(전역 핸들 표, `[ref+0x28]`). 인자가 폼·폼 목록·키워드면 읽기 |
| `ObjectReference.IsInDialogueWithPlayer` | 56194 | **쓰기** | 액터가 아닌 경로에서 같은 핸들 생성(ID 12326) |

| `FormList.Find` (17:26 추가) | 55669 | 읽기 | `HasForm`과 같은 ID 20906으로 꼬리 점프. 호출·잠금·스택 밖 쓰기 없음 |
| `Form.GetFormID` (11-11 04:11 추가) | 55902 | 읽기 | 본문이 `mov eax,[r8+0x14]; ret` (`TESForm::formID`). 등록 코드 ID 55890의 +A1F5FA에서 콜백 A20820을 확인. `[NativePacing]`이 태스크릿 실행을 허용하는 바로 그 함수이고, 그쪽은 설치할 때 이 5바이트를 직접 대조함. 11-11 06:27 빌드부터 `[NativePacing]`은 콜백 주소를 이 항목에서 가져옴(이전의 하드코딩 +0xA20820과 같은 주소임을 주소 라이브러리로 확인) |

`Form.GetFormID`는 호출 빈도로 고른 대상이 아닙니다(Save93 집계에서 20회, 맨 앞을 막은 횟수 0). `[NativePacing]`이 이 함수를 태스크릿에서 바로 실행하게 하는데 따라잡기의 판정은 그 실행을 변경으로 보아 그 스크립트의 다음 대기 호출을 막았습니다. 두 기능의 판정을 맞추려고 넣었습니다(교차 검토, Gemini).

세부 기록: [액터 상태](readlist-review/game-actor-state.md), [마법·장비·인벤토리](readlist-review/game-magic-equipment-inventory.md), [폼·참조](readlist-review/game-forms-references.md), [노드·이름·폼 목록](readlist-review/game-nodes-names-formlist.md), [콜백 디스어셈블](readlist-review/callbacks-disassembly.txt).

## 자동 프로퍼티 접근 객체 (17:26 추가)

VM은 다른 스크립트의 자동 프로퍼티에 접근할 때마다 함수 객체를 새로 만듭니다(ID 105232, 0x30바이트, 접근이 끝나면 해제). 셋 다 `GetIsNative()`가 참입니다.

| 객체 | vtable ID | 판정 | 근거 |
| --- | --- | --- | --- |
| `AutoPropGetFunction` | 252795 | 읽기 | `Call`(ID 105248)이 `obj->variables[index]`를 스택의 반환 값으로 복사(ID 104297). 스크립트 객체, 타입 정보, 프로퍼티 정보, 함수 객체에는 쓰지 않음. 잠금을 잡지 않음 |
| `AutoPropSetFunction` | 252797 | 쓰기 | `Call`(ID 105268)이 인자를 `obj->variables[index]`에 씀 |
| `NativeFunctionStub` | 252808 | 변경 취급 | 바인딩되지 않은 네이티브가 불렸다는 오류 로그만 남김 |

getter에서 반환 값 전달 말고 일어나는 일은 둘입니다. 객체 값을 복사할 때 VM의 참조 수 관리(`Object::IncRef`·`DecRef`)가 참조 수 1↔2 경계에서 핸들 정책의 표를 갱신할 수 있습니다. 이것은 getter 고유의 코드가 아니라 VM이 객체 값을 복사할 때마다 하는 일이고, 바이트코드의 대입에서도 똑같이 일어납니다. 그리고 프로세스당 한 번의 정적 초기화 가드가 있습니다. 둘 다 스크립트가 읽는 게임 상태는 아니라고 보고 읽기로 인정했습니다.

getter는 주소가 아니라 vtable로 매 호출 구분합니다. setter가 같은 할당자에서 같은 크기로 나오므로, 해제된 getter의 주소가 setter로 돌아올 수 있습니다. 직접 작성한 `Get` 함수가 있는 프로퍼티는 링커(ID 105385)가 `ScriptFunction`으로 연결하므로 `GetIsNative()`가 거짓이고, 이 객체들과 섞이지 않습니다.

읽지 않은 것: 할당자와 CRT, 오류 로그의 서식·출력 경로, 참조 수가 0이 될 때의 소멸 경로. 세부 기록: [자동 프로퍼티 접근 객체](readlist-review/vm-auto-property-accessors.md).

## SKSE64의 함수 (18개 중 읽기 14, 쓰기 4)

SKSE64 저장소의 `v2.2.8` 태그(커밋 `b9bfa27d`) 소스를 읽었습니다. 최신 소스(2.3.1)와 해당 Papyrus 파일들은 동일합니다. 설치된 DLL의 버전 표시는 2.2.8이지만, 그 소스 그대로 빌드됐는지는 확인하지 않았습니다.

| 함수 | 판정 | 근거 |
| --- | --- | --- |
| `Game.GetModByName` | 읽기 | 플러그인 목록의 이름 비교 |
| `Game.GetModName`, `Game.GetLightModName` | 읽기 | 목록에서 이름을 읽어 문자열로 반환(문자열 풀 등록) |
| `Form.GetName` | 읽기 | `TESFullName::name` 필드를 직접 읽음. 추가 데이터나 가상 함수를 거치지 않음 |
| `Actor.GetEquippedObject` | 읽기 | 프로세스 데이터의 필드 |
| `Actor.GetWornForm` | 읽기 | 추가 데이터 읽기 잠금 아래 인벤토리 순회 |
| `Armor.GetMaskForSlot` | 읽기 | 산술 연산 |
| `Spell.GetNthEffectMagicEffect` | 읽기 | 효과 목록의 n번째 |
| `MagicEffect.GetCastingArt`, `GetHitEffectArt`, `GetLight` | 읽기 | 필드 읽기 |
| `Shout.GetNthSpell`, `GetNthWordOfPower` | 읽기 | 배열의 n번째 |
| `SKSE.GetVersionRelease` | 읽기 | 상수 반환 |
| `Keyword.GetKeyword` | **쓰기** | 정적 맵이 비어 있으면 모든 키워드로 채움. SKSE 내부 캐시이고 게임 객체는 아니지만 모든 스레드가 공유함 |
| `NetImmerse.HasNode`, `GetNodeScale` | **쓰기** (조건부) | 이름 검색 가상 함수 `GetObjectByName`(슬롯 0x2A)의 대상은 실행 파일의 NiAVObject 계열 vtable 61개에서 셋. 형상 계열(ID 70257)과 노드 계열(ID 70299)은 읽기만 함. `BSFlattenedBoneTree`의 것(ID 70743)은 뼈의 노드가 없으면 NiNode를 만들어 부모에 붙이고 뼈 항목에 기록함. NPC의 뼈대는 압축 저장되므로 실제로 도달하는 경로. 뼈마다 한 번이고 이후에는 조회만 함 |
| `ObjectReference.GetDisplayName` | **쓰기** | 엔진 ID 19781: 표시 데이터가 없고 내구도가 1.0 근처가 아니면 `ExtraTextDisplayData`를 만들어 추가 데이터 목록에 넣음. 캐시된 이름이 비었거나 내구도가 달라졌으면 다시 씀. 조건에 따라 참조 핸들도 만듦(ID 12326) |

세부 기록: [SKSE64 2.2.8](readlist-review/skse64-2.2.8.md).

SKSE가 네이티브를 감싸는 공통 코드에도 부수 효과가 있습니다. 문자열 인자마다 문자열 풀 참조가 하나 남고, 폼을 반환할 때 그 폼의 VM 객체가 없으면 만들어 묶습니다. 둘 다 VM과 문자열 풀의 일이고 폼 데이터는 바꾸지 않으며, 그 호출이 언제 실행되든 똑같이 일어납니다.

## 도우미 함수

`Math`, `StringUtil`, `Debug.Trace`·`TraceStack`은 목록의 항목이 아니라 클래스 단위로 읽기로 봅니다. 함수 객체가 게임 실행 파일이나 검토한 버전의 SKSE에서 만들어졌을 때만 인정합니다. 11-11 04:11 빌드부터는 그 객체의 콜백(`+0x50`)도 같은 이미지 안을 가리켜야 합니다. 그전에는 vtable만 보았으므로, 누군가 객체는 그대로 두고 콜백만 바꿔 놓은 경우를 걸러 내지 못했습니다(교차 검토, Gemini). 게임의 `Debug.Trace`(콜백 ID 55391)·`TraceStack`(55392)·`Math.Floor`(55704)·`Math.abs`(55720)가 다른 네이티브와 같은 방식으로 `+0x50`에 콜백을 저장한다는 것은 등록 코드에서 확인했습니다. 이 검사가 게임에서 도우미 함수를 그대로 읽기로 통과시키는지는 06:45 빌드의 호출 집계로 확인했습니다(11-11 06:45~06:50, `verification/Save93-Native-Census-2026-11-11.log`): 17종 3,258회가 모두 읽기로 판정됐고 거부는 0이었습니다(`Debug.TraceStack`은 불리지 않음). 11-11 06:27 빌드부터는 이름이 목록에 있는데 구현 확인에서 거부된 함수를 로그에 남깁니다(`VM catch-up reads: … counts as a change`, 확인된 읽기는 함수마다 한 번, 도우미는 클래스마다 한 번). 그전에는 호출 집계를 돌려야만 보였습니다.

- SKSE의 `PapyrusStringUtil.cpp`와 `PapyrusMath.cpp`(2.2.8)는 직접 읽었습니다. 문자열 길이·검색·분리와 비트·로그 연산뿐이고 게임 데이터에 접근하지 않습니다.
- 게임의 `Math` 함수들(`Floor`, `abs`, `sqrt` 등)은 하나씩 읽지 않았습니다. 산술 함수라는 성격으로 인정한 것입니다.
- `Debug.Trace`·`TraceStack`은 로그 파일에 쓰지만 스크립트가 읽는 게임 상태는 바꾸지 않는다고 보았습니다. 순정에서 이미 태스크릿이 바로 실행하는 함수입니다(등록 코드 ID 55394).

## 함수 객체의 종류 (2026-11-10 16:21 정정)

`GetIsNative()`가 참인 함수 객체가 모두 네이티브 함수 객체는 아닙니다. 게임 실행 파일에는 자동 프로퍼티의 getter(vtable ID 252795)와 setter(252797), 바인딩되지 않은 네이티브의 stub(252808)이 있고, 셋 다 슬롯 8이 `mov al, 1; ret`입니다. 이 객체들에는 콜백 필드가 없습니다. 16:08 빌드의 판정 코드는 이를 구분하지 않아 객체 밖을 읽을 수 있었고, 실행 전 교차 검수에서 지적받아 고쳤습니다. 지금은 목록에 있는 이름일 때만 객체를 보고, 세 vtable을 명시적으로 제외한 뒤, 콜백을 그 항목의 주소와 비교합니다.

## 검토의 한계

- 정적 분석입니다. 실행해서 확인한 것이 아닙니다.
- 참조 수가 0이 될 때 불리는 소멸 경로는 읽지 않았습니다. 함수가 스스로 얻은 임시 참조를 놓는 경우입니다.
- 가상 함수는 실행 파일 안의 vtable만 열거했습니다. 다른 DLL이 만든 객체는 포함되지 않습니다.
- 엔진 쪽 검토 세 묶음과 SKSE 검토는 보조 에이전트가 수행했고, 본문이 필드 읽기로 끝나는 8개와 표본 세 곳(스레드별 캐시가 TLS 블록에만 쓰는 것, 핸들 생성 지점, `FormList.HasForm`의 콜백 등록)은 따로 다시 확인했습니다. 나머지 판정은 세부 기록의 ID와 RVA로 재검증할 수 있습니다.
- 처음 쓴 요약 도구(`readlist-review/review.py`, `tree.py`)는 마지막 명령이 아닌 꼬리 점프와 `rbp` 기준 쓰기를 놓칩니다. 보조 검토는 이를 보완한 자체 스크립트로 호출 트리를 따라갔습니다.
- 읽기 전용이라는 것은 "그 호출이 아무것도 바꾸지 않는다"는 뜻입니다. 두 호출 사이의 게임 갱신이 빠졌을 때 스크립트가 다른 값을 관측할 수 있다는 점은 이 검토의 대상이 아닙니다.

## 범위

호출 집계 기준으로, 처음 확인된 42개는 이름이 조회처럼 보이는 함수의 대기 호출 12,012건 중 9,867건(82%)을 덮습니다. SKSE가 2.2.8이 아니면 게임 함수 28개만 남아 6,570건(55%)입니다. 목록에 없는 것 중 큰 것은 `Actor.GetActorValue`(351건, 쓰기), `NetImmerse.HasNode`·`GetNodeScale`(각 214건, 쓰기), `ObjectReference.GetItemCount`(210건, 쓰기) 순입니다. 17:26에 더한 `FormList.Find`는 약 55건이고, 자동 프로퍼티 읽기는 대기열을 거치지 않는 직접 호출 약 5,800건(실행 한 번 기준, setter 포함)에 해당합니다.

## 쓰기로 판정한 것의 성격

쓰기로 빼 둔 함수의 대부분은 "필요할 때 한 번 만들어 두는" 지연 생성입니다. 가속은 어떤 함수도 새로 실행시키지 않으므로, 이 쓰기들은 가속이 없어도 한 프레임 뒤에 똑같이 일어납니다. 읽기 판정이 실제로 가르는 것은 "이 호출 뒤에 같은 스택의 다음 호출을 게임 갱신 없이 이어서 실행해도 되는가"입니다. 그 관점에서 함수별로 따져야 할 것은 쓰기의 유무보다 다음과 같습니다.

| 함수 | 쓰는 것 | 게임이 따로 처리하는가 | 세이브에 남는가 | 따져 볼 점 |
| --- | --- | --- | --- | --- |
| `NetImmerse.HasNode`, `GetNodeScale` | 뼈 노드 생성·부착 | 아니오 | 아니오(3D) | 잠금 없이 장면 그래프를 바꿈. 순정 호출도 같은 노출 |
| `Actor.GetActorValue`, `GetActorValuePercentage` | 값 캐시 다시 채움 | 아니오 | 아니오 | 잠금 없는 캐시 갱신 |
| `Actor.GetBaseActorValue` (위 둘도 도달) | 없는 기본값을 액터의 값 표에 삽입 | 아니오 | 가능성 있음 | 전역 쓰기 잠금 |
| `Actor.HasSpell` (주문 인자) | 레벨드 주문 목록 생성 | 아니오 | 확인 안 함 | 내용이 결정적인지 읽지 않음 |
| `ObjectReference.GetItemCount`, `IsInDialogueWithPlayer` | 참조 핸들 생성 | 아니오 | 아니오 | 전역 핸들 표, 해제되지 않는 참조 수 |
| `ObjectReference.GetDisplayName` | 표시 데이터 생성, 이름 캐시 | 아니오 | 예 | 추가 데이터가 세이브에 기록됨 |
| `Keyword.GetKeyword` | SKSE 내부 캐시 채움 | 아니오 | 아니오 | 플러그인이 미리 한 번 불러 두면 이후는 조회만 |

이 표는 검토 기록에서 뽑은 정리이고, "세이브에 남는가"는 따로 검증하지 않은 추정을 포함합니다. 이런 지연 생성을 읽기로 받아들일지는 정하지 않았습니다.

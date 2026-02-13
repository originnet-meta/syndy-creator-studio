# 3D 모델(.glb/.gltf + Draco) OBS 입력 소스 MVP 개발 계획 (Windows + D3D11 우선)

## 1. Scope & Non-Goals

### Scope (MVP)
- Windows + D3D11 환경에서 신규 입력 소스(`model3d_source` 가칭) 추가.
- 로컬 디스크 `.glb/.gltf` 파일(Draco 압축 포함) 선택 후 메시(단일/복수 primitive) + baseColor 텍스처 렌더링.
- 알파 블렌딩/컴포지팅은 OBS 표준 소스 렌더 경로를 사용.
- 색공간은 `sRGB 텍스처 샘플링 + linear 셰이딩 + framebuffer sRGB` 규칙을 준수.
- 렌더 스레드 블로킹 방지를 위해 비동기 로딩(파일 I/O + glTF 파싱 + Draco 디코드) / 그래픽 컨텍스트 업로드 분리.

### Non-Goals (Phase 2로 이관)
- 애니메이션, 스키닝, 모프 타깃.
- 풀 PBR(MR/SpecGloss), normal/occlusion/emissive 고급 머티리얼.
- IBL, 그림자, 포스트프로세싱.
- OpenGL/Metal 백엔드 동등 구현.

---

## 2. Architecture Overview (D3D11 우선)

```text
[OBS Frontend: 소스 속성 UI]
   └─ file path(.glb/.gltf, Draco 포함), scale/orientation 옵션
      └─ obs_source_update()
          └─ [Loader Worker Thread]
              ├─ glTF JSON/GLB chunk parse + KHR_draco_mesh_compression 해석
              ├─ CPU mesh/index/uv buffer 생성
              └─ decode image(baseColor) -> CPU RGBA
                  └─ lock-free/뮤텍스 큐로 "업로드 요청" 전달

[OBS Video/Render Thread]
   └─ video_tick / video_render
      ├─ obs_enter_graphics() 구간에서 GPU 리소스 생성
      │   ├─ gs_vertexbuffer_create / gs_indexbuffer_create
      │   ├─ gs_texture_create (sRGB view)
      │   └─ gs_effect_create_from_file (plugin effect)
      ├─ gs_effect_set_texture_srgb(image, tex)
      ├─ gs_load_vertexbuffer + gs_draw
      └─ OBS scene item transform 경로로 최종 합성

[GS 추상화]
   └─ gs_* API
      └─ Windows MVP에서는 libobs-d3d11 device_* 구현으로 매핑
         (SRV linear/sRGB 분리, D3D11 shader/resource 생성)
```

핵심 포인트: 구현은 `gs_*` 추상화 중심으로 작성하되, 검증/리스크 관리는 D3D11 매핑(`device_*`, SRV linear/sRGB 분기, device loss callback) 기준으로 상세화한다.

---

## 3. Key Decisions

1. **신규 플러그인 방식 채택 (`plugins/model3d-source`)**
   - 이유: 기존 `image-source`가 입력 소스 + 파일 프로퍼티 + GPU upload 수명주기 패턴을 이미 보유.
   - 대안: `image-source` 확장(리스크: 책임 과대, 회귀 영향 증가).

2. **로더는 cgltf + Draco Decoder 조합 우선 도입**
   - 이유: Draco 압축 메시(KHR_draco_mesh_compression) 대응이 목표이며, C API 기반으로 통합/성능 제어가 용이.
   - 대안: assimp/tinygltf 단독(Draco 처리 추가 작업 필요, 의존성/빌드 비용 증가).

3. **렌더 경로는 OBS 기본 소스 렌더 계약 준수**
   - `obs_source_info.video_render`에서 드로우, `OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB` 사용.
   - 필요 시 `OBS_SOURCE_CUSTOM_DRAW`를 사용해 effect NULL 경로를 명확화.

4. **색공간 정책**
   - baseColor 텍스처는 sRGB 샘플링(`gs_effect_set_texture_srgb`) 사용.
   - framebuffer sRGB on/off는 기존 이미지/전환 소스 패턴과 동일하게 push/pop.

5. **스레딩 정책**
   - 파싱/디코드: worker thread.
   - GPU 객체 생성/파괴: 반드시 `obs_enter_graphics()/obs_leave_graphics()` 구간.
   - 전달: double-buffered pending payload + 원자 플래그/뮤텍스.

6. **D3D11 우선 검증**
   - Windows renderer 설정에서 Direct3D 11 선택 경로를 1차 기준.
   - `libobs-d3d11`의 texture SRV linear/sRGB 분리를 기반으로 색 정확도 검증.

---

## 4. Dependencies (Build / Runtime / Platform)

### Platform (MVP)
- Windows 10/11 x64
- OBS renderer: Direct3D 11 (`libobs-d3d11`)

### Build-time
- 신규 plugin target: `plugins/model3d-source/CMakeLists.txt`
- glTF parser: `cgltf` (single-header/source vendoring)
- Draco decoder: Google Draco 라이브러리(또는 동등 decoder) 정적 링크/서브모듈 도입
- 이미지 디코더: 기존 libobs image path 재사용 가능성 우선 검토, 필요 시 plugin-local decoder 최소화

### Runtime
- `.glb/.gltf` 파일(Draco extension 포함)
- baseColor 텍스처 파일(외부 URI 또는 GLB embedded)

### License/Governance
- OBS GPL-2.0+ 정책과 호환 가능한 의존성만 허용.
- 3rdparty 도입 시 SPDX/license file 포함 + CMake 옵션화.

---

## 5. 코드 근거 기반 조사 결과

| 영역 | 파일 경로 | 함수/심볼 | 요약(왜 중요한지) | 관련 Task/Impl |
|---|---|---|---|---|
| source 등록 | `libobs/obs-module.c` | `obs_register_source_s` | 신규 source 타입 등록 검증 규칙(필수 callback/flag 제약) 기준점. | BLD-01, RND-01, IMP-01 |
| source 플래그 계약 | `libobs/obs-source.h` | `OBS_SOURCE_VIDEO`, `OBS_SOURCE_CUSTOM_DRAW`, `OBS_SOURCE_SRGB`, `struct obs_source_info` | source output flag/렌더 계약 정의. MVP source 선언 시 직접 참조 필요. | RND-01, IMP-03 |
| 입력 소스 레퍼런스 | `plugins/image-source/image-source.c` | `image_source_info`, `image_source_properties`, `image_source_render`, `obs_module_load` | 파일 프로퍼티, 소스 등록, 렌더 및 graphics enter/leave 패턴이 가장 유사. | UX-01, RND-02, IMP-02 |
| 장면 합성/변환 | `libobs/obs-scene.c` | `render_item`, `scene_video_render` | scene item transform/texcoords/합성 경로가 자동 적용됨. 신규 소스는 표준 비디오 소스로 붙이면 됨. | RND-01, TST-03, IMP-04 |
| 소스 렌더 호출 체인 | `libobs/obs-source.c` | `obs_source_video_render`, `render_video`, `obs_source_main_render`, `source_render` | source의 video_render가 합성 파이프라인에서 호출되는 실제 경로. | RND-01, IMP-03 |
| graphics context 규칙 | `libobs/obs.c`, `libobs/graphics/graphics.c` | `obs_enter_graphics`, `obs_leave_graphics`, `gs_enter_context`, `gs_leave_context` | GPU 리소스 생성/파괴 thread-safety 규칙 근거. | LDR-03, RND-03, IMP-05 |
| 효과/셰이더 로딩 | `libobs/obs.c`, `libobs/graphics/graphics.c` | `obs_load_effect`, `gs_effect_create_from_file`, `gs_create` | effect 파일 로딩 및 graphics backend 모듈 생성 진입점. | RND-02, BLD-02, IMP-03 |
| D3D11 backend 매핑 | `libobs-d3d11/d3d11-subsystem.cpp` | `device_create`, `device_texture_create`, `device_vertexbuffer_create`, `device_draw` | gs_* 호출이 D3D11 실제 API로 연결되는 핵심 경로. | RND-04, TST-02, IMP-06 |
| D3D11 sRGB/linear SRV | `libobs-d3d11/d3d11-texture2d.cpp` | `gs_texture_2d::InitResourceView` | 동일 texture에 linear/sRGB SRV를 준비하는 구현. 색공간 정확성 검증의 핵심. | RND-04, TST-02, IMP-06 |
| device loss 대응 | `libobs-d3d11/d3d11-subsystem.cpp` | `device_register_loss_callbacks`, `device_unregister_loss_callbacks` | 디바이스 손실/재생성 이벤트 대응 확장 포인트. | RND-05, TST-04, IMP-06 |
| 프로퍼티 API | `libobs/obs-properties.h` | `obs_properties_add_path`, `obs_properties_add_bool`, modified callback API | 파일 다이얼로그/옵션 UI 정의를 위한 표준 API 근거. | UX-01, UX-02, IMP-02 |
| plugin 빌드 구조 | `plugins/CMakeLists.txt`, `plugins/image-source/CMakeLists.txt` | `add_obs_plugin(image-source)`, module target 패턴 | 신규 plugin 추가/빌드 연결의 기준 템플릿. | BLD-01, BLD-02, IMP-01 |
| 로케일/리소스 구조 | `plugins/image-source/data/locale/*.ini`, `plugins/image-source/image-source.c` | `OBS_MODULE_USE_DEFAULT_LOCALE` | 신규 plugin locale key/번역 파일 배치 규칙 근거. | UX-03, IMP-02 |
| 테스트 하네스 | `test/CMakeLists.txt`, `test/test-input/test-input.c` | `add_subdirectory(test-input)`, `obs_register_source` 테스트 모듈 | smoke 테스트용 소스 모듈 구성 방식/추가 위치 근거. | TST-01, TST-03, IMP-07 |

추가 조사 결론:
- 레포 전역 검색 기준 `tinygltf/cgltf/assimp/gltf/glb/draco` 관련 기존 코드 참조는 확인되지 않음(신규 도입 필요).

---

## 6. Task List

### 빌드 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| BLD-01 | 빌드 | 신규 플러그인 골격 추가 | `plugins/model3d-source` 생성, module entry + CMake 타깃 추가. 기존 image-source 패턴 재사용으로 리스크 축소. | 없음 | Windows 빌드에서 plugin dll 생성, 모듈 로드 로그 확인 | `plugins/CMakeLists.txt:add_obs_plugin` / `plugins/image-source/CMakeLists.txt` / `plugins/image-source/image-source.c:obs_module_load` |
| BLD-02 | 빌드 | effect/locale/data 설치 경로 구성 | plugin data 하위에 `.effect`, locale ini, 기본 샘플 에셋 경로 구성. 런타임 `obs_find_data_file` 규칙과 정합. | BLD-01 | 실행 시 effect/locale 로드 성공, locale key 표시 | `libobs/obs.c:obs_load_effect` / `plugins/image-source/image-source.c:OBS_MODULE_USE_DEFAULT_LOCALE` |
| BLD-03 | 빌드 | Draco 디코더 + glTF 파서 도입 및 라이선스 정리 | cgltf + Draco decoder 3rdparty를 plugin-local 또는 deps 하위 도입, 라이선스 파일 및 CMake 옵션화. | BLD-01 | clean build + NOTICE/라이선스 검토 체크리스트 통과 | 전역 검색 결과(기존 glTF/Draco 파서 부재), `deps/` 구조, `plugins/*/CMakeLists.txt` 패턴 |

### 로더 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| LDR-01 | 로더 | `.gltf/.glb` 입력 + Draco 확장 감지 | properties path 기준 base dir 계산, 외부 텍스처 URI/GLB embedded 처리와 함께 `KHR_draco_mesh_compression` 존재 여부를 판별. | BLD-03, UX-01 | 샘플 glTF/GLB(Draco 포함) 로드 성공(수동) | `plugins/image-source/image-source.c:image_source_properties` / `libobs/obs-properties.h:obs_properties_add_path` |
| LDR-02 | 로더 | Draco 메시 디코드 후 CPU 버퍼 생성 | Draco extension이 있으면 decoder로 포지션/인덱스/UV를 복원하고, 미지원 속성은 warning + graceful skip. | LDR-01 | 유효 Draco 모델 화면 출력, 디코드 실패 시 crash 없음 | `plugins/text-freetype2/obs-convenience.c:create_uv_vbuffer`(VB 구조 참고) |
| LDR-03 | 로더 | 비동기 파싱 파이프라인 | worker thread에서 I/O+파싱 수행, render thread는 pending payload polling만 수행하여 프레임 블로킹 방지. | LDR-01 | 대형 모델 로드시 렌더 스레드 hitch 최소화(프레임 타임 기준) | `plugins/image-source/image-source.c:file_decoded/texture_loaded + tick` 패턴 |
| LDR-04 | 로더 | 이미지 디코드/알파 준비 | baseColor 텍스처 디코드 후 RGBA 업로드 데이터 준비, premultiplied 정책 명시. | LDR-01 | 반투명 텍스처 가장자리 halo 없음 | `plugins/image-source/image-source.c:linear_alpha, gs_image_file4_init` |

### 렌더 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| RND-01 | 렌더 | source 타입/콜백 계약 정의 | `obs_source_info`에 create/destroy/update/get_width/get_height/video_render/video_tick/get_properties 등록. | BLD-01 | 소스 생성/삭제/장면 추가 정상 | `libobs/obs-module.c:obs_register_source_s` / `libobs/obs-source.h:struct obs_source_info` |
| RND-02 | 렌더 | 모델 전용 effect + draw path 구현 | MVP unlit texture effect 작성, texture param + transform 상수 설정, `gs_draw` 호출. | BLD-02, LDR-02 | 단일 mesh 출력 + 알파 합성 정상 | `libobs/graphics/graphics.c:gs_effect_create_from_file` / `plugins/obs-transitions/transition-luma-wipe.c:luma_wipe_callback` |
| RND-03 | 렌더 | GPU 업로드 수명주기 | pending CPU payload를 `obs_enter_graphics` 구간에서 VB/IB/texture로 생성/교체/해제. destroy에서도 동일 규칙 적용. | LDR-03, LDR-04 | 리소스 누수/크래시 없음(반복 소스 on/off) | `libobs/obs.c:obs_enter_graphics` / `libobs/graphics/graphics.c:gs_enter_context` / `plugins/image-source/image-source.c:image_source_unload` |
| RND-04 | 렌더 | sRGB/linear 색공간 정합 | baseColor는 `gs_effect_set_texture_srgb`, framebuffer sRGB 제어 및 source flags에 `OBS_SOURCE_SRGB` 반영. | RND-02 | 색상 비교 기준 통과(레퍼런스 이미지 대비) | `plugins/image-source/image-source.c:image_source_render` / `libobs/obs-source.c:obs_source_main_render` / `libobs-d3d11/d3d11-texture2d.cpp:InitResourceView` |
| RND-05 | 렌더 | D3D11 device-loss/재초기화 대응 설계 | MVP는 안전 최소치(재로딩 트리거 + null-safe draw), 후속으로 loss callback 연결. | RND-03 | 디바이스 재초기화/그래픽스 재시작 후 크래시 없음 | `libobs-d3d11/d3d11-subsystem.cpp:device_register_loss_callbacks` |

### UX 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| UX-01 | UX | 파일 선택 프로퍼티 | `obs_properties_add_path`로 `*.glb *.gltf` 필터 제공 + Draco 지원 안내 문구 제공. | RND-01 | 속성창에서 파일 선택 가능, Draco 지원 안내 노출 | `libobs/obs-properties.h:obs_properties_add_path` / `plugins/image-source/image-source.c:image_filter` |
| UX-02 | UX | 로딩 상태/오류 메시지 및 옵션 | flipY, cull mode, alpha mode(기본/프리멀티) 최소 옵션 추가 + 잘못된 파일 경고 로그. | UX-01, LDR-01 | 잘못된 파일에서 사용자 이해 가능한 오류 | `libobs/obs-properties.h:obs_properties_add_bool` / `plugins/image-source/image-source.c:warn` |
| UX-03 | UX | 로케일 키/번역 파일 추가 | `data/locale/en-US.ini` 우선 + 기존 locale 확장 포맷 준수. | BLD-02 | UI 문자열 key 누락 없음 | `plugins/image-source/data/locale/*.ini` / `plugins/image-source/image-source.c:OBS_MODULE_USE_DEFAULT_LOCALE` |

### 테스트 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| TST-01 | 테스트 | 빌드/로드 스모크 | Windows 빌드 + OBS 실행 시 모듈 로드/소스 생성 smoke 체크(Draco ON/OFF 빌드 옵션 포함). | BLD-01~03 | plugin load fail 0건 | `test/CMakeLists.txt` / `test/win/*` 모듈 로드 테스트 구조 |
| TST-02 | 테스트 | D3D11 색공간 정확성 검증 | sRGB 텍스처 샘플링/알파 합성 스냅샷 비교(레퍼런스 패턴 모델). | RND-04 | ΔE 또는 픽셀 diff 임계치 이내 | `libobs-d3d11/d3d11-texture2d.cpp:InitResourceView` / `plugins/image-source/image-source.c:image_source_render` |
| TST-03 | 테스트 | scene 합성/변환 회귀 | scene item 위치/스케일/회전 변경 시 모델 소스가 표준 transform 경로로 동작하는지 검증. | RND-01~04 | 변환 시 왜곡/좌표 이탈 없음 | `libobs/obs-scene.c:render_item` / `libobs/obs-source.c:obs_source_video_render` |
| TST-04 | 테스트 | 수명주기/안정성 | 소스 반복 생성/삭제, 파일 교체, 장면 전환, 장시간 실행, device reset 시나리오 검증. | RND-03, RND-05 | crash/leak 없음, GPU 메모리 안정 | `plugins/image-source/image-source.c:image_source_destroy` / `libobs-d3d11/d3d11-subsystem.cpp:device_register_loss_callbacks` |

---

## 7. Implementation Plans (IMP-01 ~ IMP-08)

## [IMP-01] 플러그인 스캐폴딩 + 빌드 타깃 연결

### 구현 요약

- **작업 진입점**: `plugins/CMakeLists.txt`, `plugins/image-source/CMakeLists.txt`, `plugins/image-source/image-source.c:obs_module_load`
- **핵심 변경**: `plugins/model3d-source` 신규 모듈 골격 생성, plugin target/data/locale 기본 구조 연결
- **등록 지점**: `obs_module_load`에서 `obs_register_source` 호출 구조 확립
- **가드/호환성**: 기존 플러그인 로딩 순서/동작 불변, 신규 모듈 미사용 시 영향 없음
- **테스트**: Windows 빌드 1회 + 모듈 로드 로그 확인

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-01
prompt: |
  syndy-creator-studio 레포에서 IMP-01만 수행한다.
  목표:
  - plugins/model3d-source 스캐폴딩을 추가하고 CMake에 연결한다.
  - 모듈 엔트리(obs_module_load)에서 source 등록 구조를 만든다.
  작업 순서:
  1) plugins/image-source 패턴으로 폴더/파일 골격 생성
  2) plugins/CMakeLists.txt에 add_obs_plugin 항목 추가
  3) model3d-source CMake와 locale/effect 데이터 경로 연결
  4) 빌드로 모듈 로딩 가능 여부 확인
```

---

## [IMP-02] 설정/프로퍼티 UI + Draco 지원 옵션 추가

### 구현 요약

- **작업 진입점**: `libobs/obs-properties.h:obs_properties_add_path`, `plugins/image-source/image-source.c:image_source_properties`
- **핵심 변경**: `*.glb *.gltf` 파일 선택 + Draco decode policy 옵션 + 상태/오류 메시지 키 추가
- **콜백 지점**: `update`에서 settings 반영, 필요 시 modified callback으로 동적 UI 토글
- **가드/호환성**: 옵션 미설정 시 안전한 기본값으로 동작
- **테스트**: 속성창 열기/저장/재열기 및 잘못된 경로 입력 경고 확인

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-02
prompt: |
  syndy-creator-studio 레포에서 IMP-02만 수행한다.
  목표:
  - model3d source 프로퍼티에 glb/gltf 파일 선택과 Draco 관련 옵션을 추가한다.
  작업 순서:
  1) properties 함수에 파일 필터와 bool/list 옵션 추가
  2) defaults/update 경로에 설정 키를 연결
  3) locale 문자열 키 추가(en-US 우선)
  4) 경로 오류/미지원 포맷 warning 메시지 정리
```

---

## [IMP-03] source 수명주기/렌더 루프 기본 계약 구현

### 구현 요약

- **작업 진입점**: `libobs/obs-source.h:struct obs_source_info`, `libobs/obs-module.c:obs_register_source_s`
- **핵심 변경**: `create/destroy/update/get_width/get_height/video_tick/video_render` 기본 루프 구현
- **렌더 계약**: `OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB` 기준으로 draw 진입 경로 확보
- **가드/호환성**: resource null-check 및 빈 프레임에서 no-op draw
- **테스트**: 소스 생성/삭제 반복 시 crash-free 확인

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-03
prompt: |
  syndy-creator-studio 레포에서 IMP-03만 수행한다.
  목표:
  - model3d source의 obs_source_info 계약과 수명주기 콜백을 구현한다.
  작업 순서:
  1) source context 구조체 정의
  2) 필수/핵심 콜백 연결
  3) 기본 effect 로딩과 video_render 골격 작성
  4) width/height 반환 정책 확정
```

---

## [IMP-04] Draco 기반 glTF/GLB CPU 로더 구현

### 구현 요약

- **작업 진입점**: 신규 `plugins/model3d-source/model3d-loader.*`, 참고 `plugins/text-freetype2/obs-convenience.c`
- **핵심 변경**: `KHR_draco_mesh_compression` 우선 디코드 + non-Draco accessor fallback
- **디코드 경로**: positions/uv0/indices 복원, baseColor 텍스처 URI/embedded 처리
- **가드/호환성**: 디코드 실패 시 명시적 오류 반환/로그, 미지원 속성 graceful skip
- **테스트**: Draco 샘플/비Draco 샘플 각각 로드 성공

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-04
prompt: |
  syndy-creator-studio 레포에서 IMP-04만 수행한다.
  목표:
  - cgltf + Draco decoder로 KHR_draco_mesh_compression primitive를 디코드한다.
  - extension이 없는 primitive는 accessor 경로로 fallback 처리한다.
  작업 순서:
  1) glTF 파서 래퍼 작성
  2) Draco extension 감지 및 비트스트림 decode
  3) CPU payload(정점/인덱스/UV/텍스처) 구성
  4) 오류코드/로그 표준화
```

---

## [IMP-05] 비동기 로드 → 렌더 스레드 업로드 브리지

### 구현 요약

- **작업 진입점**: `plugins/image-source/image-source.c:file_decoded/texture_loaded`, `libobs/obs.c:obs_enter_graphics`
- **핵심 변경**: worker thread parse/decode, render tick에서 GPU 업로드 state machine 구축
- **업로드 지점**: `obs_enter_graphics` 구간에서 VB/IB/texture create/swap/free
- **가드/호환성**: 취소 토큰, source destroy 시 join 순서 보장
- **테스트**: 대형 모델 로드시 프레임 hitch 감소 및 반복 로드 안정성 확인

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-05
prompt: |
  syndy-creator-studio 레포에서 IMP-05만 수행한다.
  목표:
  - 로더 비동기화와 render-thread GPU 업로드 브리지를 구현한다.
  작업 순서:
  1) worker job queue + cancel token 추가
  2) pending payload 동기화 구조 구현
  3) video_tick에서 업로드 트리거
  4) destroy 시 thread/resource 정리 순서 고정
```

---

## [IMP-06] 색공간/알파 규칙 정합 + D3D11 검증 포인트 추가

### 구현 요약

- **작업 진입점**: `plugins/image-source/image-source.c:image_source_render`, `libobs-d3d11/d3d11-texture2d.cpp:InitResourceView`
- **핵심 변경**: `gs_effect_set_texture_srgb` 적용, framebuffer sRGB push/pop, premultiplied 알파 규칙 고정
- **D3D11 관점**: linear/sRGB SRV 분리 동작을 기준으로 결과 검증
- **가드/호환성**: 색공간 미지정 시 안전 기본값 적용
- **테스트**: 기준 패턴 이미지 캡처 비교(색편차/알파 halo)

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-06
prompt: |
  syndy-creator-studio 레포에서 IMP-06만 수행한다.
  목표:
  - model3d source 색공간/알파 처리를 D3D11 우선으로 정합시킨다.
  작업 순서:
  1) texture 파라미터 sRGB setter 적용
  2) framebuffer sRGB 토글 규칙 표준화
  3) 알파 blend state 검토/보정
  4) D3D11 기준 시각 검증 체크포인트 문서화
```

---

## [IMP-07] 디바이스 리셋/수명주기 안정화

### 구현 요약

- **작업 진입점**: `libobs-d3d11/d3d11-subsystem.cpp:device_register_loss_callbacks`, 소스 destroy/update 경로
- **핵심 변경**: device-loss 시 재업로드 트리거와 null-safe draw 경로 정립
- **수명주기 포인트**: scene 전환/소스 제거/파일 교체 시 리소스 누수 방지
- **가드/호환성**: 콜백 미등록 또는 손실 이벤트 부재 시 기존 경로 유지
- **테스트**: 장면 전환/소스 재생성/그래픽 초기화 반복 안정성 확인

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-07
prompt: |
  syndy-creator-studio 레포에서 IMP-07만 수행한다.
  목표:
  - D3D11 device loss/재초기화 상황에서 model3d source 수명주기를 안정화한다.
  작업 순서:
  1) 손실 이벤트 수신/해제 경로 설계
  2) GPU 리소스 재생성 트리거 연결
  3) draw null-guard 및 상태 복구 처리
  4) 반복 시나리오 안정성 점검
```

---

## [IMP-08] 테스트 하네스/검증 시나리오 통합

### 구현 요약

- **작업 진입점**: `test/CMakeLists.txt`, `test/test-input/test-input.c`, `test/win/*`
- **핵심 변경**: Draco ON/OFF 빌드 스모크, scene transform 회귀, 장시간 안정성 체크리스트 정리
- **자동/수동 분리**: 자동 smoke + 수동 시각 품질 검증으로 단계화
- **가드/호환성**: 테스트 미실행 환경에서도 기본 빌드/로드 검증은 수행 가능
- **테스트**: CI 후보 시나리오 및 로컬 재현 명령 문서화

### Codex Task Prompt (붙여넣기용)

```text
TASK:
id: IMP-08
prompt: |
  syndy-creator-studio 레포에서 IMP-08만 수행한다.
  목표:
  - model3d source에 대한 Draco 중심 테스트/검증 시나리오를 정리하고 하네스에 연결한다.
  작업 순서:
  1) smoke/회귀/성능 시나리오 분류
  2) test-input 기반 자동 검증 포인트 정의
  3) D3D11 수동 시각 검증 체크리스트 작성
  4) CI 적용 가능한 최소 실행 세트 제안
```

---

## 8. Risk Register + Mitigations

| 리스크 | 영향 | 가능성 | 대응 방안(Mitigation) | 관련 Task/Impl |
|---|---|---|---|---|
| D3D11 색공간 처리 오류(sRGB/linear 혼선) | 색 왜곡/알파 테두리 | 중 | `gs_effect_set_texture_srgb` 강제, framebuffer_srgb 토글 표준화, 레퍼런스 스냅샷 테스트 | RND-04, TST-02, IMP-06 |
| 렌더 스레드 블로킹(대형 모델 파싱) | 프레임 드랍/UI 멈춤 | 높음 | worker parse + render-thread upload 분리, 파일 변경 debounce | LDR-03, IMP-05 |
| GPU 리소스 수명주기 결함 | 크래시/메모리 누수 | 중 | 생성/파괴를 graphics context로 제한, destroy/join 순서 고정 | RND-03, TST-04, IMP-05 |
| 디바이스 손실/재초기화 미대응 | 출력 중단/검은 화면 | 중 | MVP는 재업로드 트리거 + null-safe draw, Phase 2에서 loss callback 정식 연동 | RND-05, IMP-06 |
| 라이선스 비호환 3rdparty 도입 | 배포 불가 | 중 | GPL-2.0 호환성 검토 체크리스트, NOTICE/SPDX 명시 | BLD-03, IMP-04 |
| Draco 디코더 통합 복잡도 과소평가 | 일정 지연/불안정성 | 높음 | MVP 스키마 고정(메시+baseColor), decoder API 래퍼 계층 분리, 실패 시 명확한 fallback 정책 적용 | BLD-03, LDR-02, IMP-04 |

---

## 9. Follow-up / Phase 2 backlog

### Backend 확장
- OpenGL 경로 정합성 검증(`libobs-opengl`) 및 셰이더 분기.
- Metal 경로 정합성 검증(`libobs-metal`) 및 macOS 테스트 매트릭스 추가.

### 기능 확장
- KHR_mesh_quantization/EXT_meshopt_compression 대응 확장.
- PBR(Metallic-Roughness), normal/occlusion/emissive 맵.
- 스키닝/애니메이션/모프 타깃.
- 다중 머티리얼/서브메시 배치 최적화.
- IBL/환경광/간단한 그림자.
- 카메라/라이트 노드 해석, gizmo 연동(편집 UX).

### 성능/운영
- mesh/texture 캐시(동일 파일 재사용).
- 백그라운드 프리페치, async cancel 고도화.
- GPU 메모리 budget 추적 및 진단 오버레이.

# 3D 모델(.glb/.gltf) OBS 입력 소스 MVP 개발 계획 (Windows + D3D11 우선)

## 1. Scope & Non-Goals

### Scope (MVP)
- Windows + D3D11 환경에서 신규 입력 소스(`model3d_source` 가칭) 추가.
- 로컬 디스크 `.glb/.gltf` 파일 선택 후 메시(단일/복수 primitive) + baseColor 텍스처 렌더링.
- 알파 블렌딩/컴포지팅은 OBS 표준 소스 렌더 경로를 사용.
- 색공간은 `sRGB 텍스처 샘플링 + linear 셰이딩 + framebuffer sRGB` 규칙을 준수.
- 렌더 스레드 블로킹 방지를 위해 비동기 로딩(파일 I/O + 파싱) / 그래픽 컨텍스트 업로드 분리.

### Non-Goals (Phase 2로 이관)
- 애니메이션, 스키닝, 모프 타깃.
- 풀 PBR(MR/SpecGloss), normal/occlusion/emissive 고급 머티리얼.
- IBL, 그림자, 포스트프로세싱.
- OpenGL/Metal 백엔드 동등 구현.

---

## 2. Architecture Overview (D3D11 우선)

```text
[OBS Frontend: 소스 속성 UI]
   └─ file path(.glb/.gltf), scale/orientation 옵션
      └─ obs_source_update()
          └─ [Loader Worker Thread]
              ├─ glTF JSON/GLB chunk parse
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

2. **로더는 경량 C 기반 파서(cgltf 계열) 우선 도입**
   - 이유: MVP 목표(geometry + baseColor)에 충분, 통합 난이도/빌드 복잡도 낮음.
   - 대안: assimp/tinygltf(의존성 크기/빌드시간/라이선스 검토 비용 증가).

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
- glTF parser(후보): `cgltf` (single-header/source vendoring)
- 이미지 디코더: 기존 libobs image path 재사용 가능성 우선 검토, 필요 시 plugin-local decoder 최소화

### Runtime
- `.glb/.gltf` 파일
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
- 레포 전역 검색 기준 `tinygltf/cgltf/assimp/gltf/glb` 관련 기존 코드 참조는 확인되지 않음(신규 도입 필요).

---

## 6. Task List

### 빌드 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| BLD-01 | 빌드 | 신규 플러그인 골격 추가 | `plugins/model3d-source` 생성, module entry + CMake 타깃 추가. 기존 image-source 패턴 재사용으로 리스크 축소. | 없음 | Windows 빌드에서 plugin dll 생성, 모듈 로드 로그 확인 | `plugins/CMakeLists.txt:add_obs_plugin` / `plugins/image-source/CMakeLists.txt` / `plugins/image-source/image-source.c:obs_module_load` |
| BLD-02 | 빌드 | effect/locale/data 설치 경로 구성 | plugin data 하위에 `.effect`, locale ini, 기본 샘플 에셋 경로 구성. 런타임 `obs_find_data_file` 규칙과 정합. | BLD-01 | 실행 시 effect/locale 로드 성공, locale key 표시 | `libobs/obs.c:obs_load_effect` / `plugins/image-source/image-source.c:OBS_MODULE_USE_DEFAULT_LOCALE` |
| BLD-03 | 빌드 | glTF parser 도입 및 라이선스 정리 | cgltf(또는 동급) 3rdparty를 plugin-local 또는 deps 하위 도입, 라이선스 파일 및 CMake 옵션화. | BLD-01 | clean build + NOTICE/라이선스 검토 체크리스트 통과 | 전역 검색 결과(기존 glTF 파서 부재), `deps/` 구조, `plugins/*/CMakeLists.txt` 패턴 |

### 로더 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| LDR-01 | 로더 | `.gltf/.glb` 파일 입력/경로 해석 | properties path에서 받은 파일 기준 base dir 계산, 외부 텍스처 URI 해석, GLB embedded 이미지 처리. | BLD-03, UX-01 | 샘플 glTF/GLB 모두 로드 성공(수동) | `plugins/image-source/image-source.c:image_source_properties` / `libobs/obs-properties.h:obs_properties_add_path` |
| LDR-02 | 로더 | CPU 메시/인덱스/UV 버퍼 생성 | MVP 스키마(positions, normals optional, texcoord0)만 우선 지원. unsupported 속성은 warning + graceful skip. | LDR-01 | 유효 모델에서 화면 출력, 잘못된 accessor에서 crash 없음 | `plugins/text-freetype2/obs-convenience.c:create_uv_vbuffer`(VB 구조 참고) |
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
| UX-01 | UX | 파일 선택 프로퍼티 | `obs_properties_add_path`로 `*.glb *.gltf` 필터 제공. | RND-01 | 속성창에서 파일 선택 가능 | `libobs/obs-properties.h:obs_properties_add_path` / `plugins/image-source/image-source.c:image_filter` |
| UX-02 | UX | 로딩 상태/오류 메시지 및 옵션 | flipY, cull mode, alpha mode(기본/프리멀티) 최소 옵션 추가 + 잘못된 파일 경고 로그. | UX-01, LDR-01 | 잘못된 파일에서 사용자 이해 가능한 오류 | `libobs/obs-properties.h:obs_properties_add_bool` / `plugins/image-source/image-source.c:warn` |
| UX-03 | UX | 로케일 키/번역 파일 추가 | `data/locale/en-US.ini` 우선 + 기존 locale 확장 포맷 준수. | BLD-02 | UI 문자열 key 누락 없음 | `plugins/image-source/data/locale/*.ini` / `plugins/image-source/image-source.c:OBS_MODULE_USE_DEFAULT_LOCALE` |

### 테스트 트랙

| ID | 트랙 | Task 요약 | 상세 설명(무엇/왜/어떻게) | 의존성 | 검증/AC | 코드 근거(Evidence) |
|---|---|---|---|---|---|---|
| TST-01 | 테스트 | 빌드/로드 스모크 | Windows 빌드 + OBS 실행 시 모듈 로드/소스 생성 smoke 체크. | BLD-01~03 | plugin load fail 0건 | `test/CMakeLists.txt` / `test/win/*` 모듈 로드 테스트 구조 |
| TST-02 | 테스트 | D3D11 색공간 정확성 검증 | sRGB 텍스처 샘플링/알파 합성 스냅샷 비교(레퍼런스 패턴 모델). | RND-04 | ΔE 또는 픽셀 diff 임계치 이내 | `libobs-d3d11/d3d11-texture2d.cpp:InitResourceView` / `plugins/image-source/image-source.c:image_source_render` |
| TST-03 | 테스트 | scene 합성/변환 회귀 | scene item 위치/스케일/회전 변경 시 모델 소스가 표준 transform 경로로 동작하는지 검증. | RND-01~04 | 변환 시 왜곡/좌표 이탈 없음 | `libobs/obs-scene.c:render_item` / `libobs/obs-source.c:obs_source_video_render` |
| TST-04 | 테스트 | 수명주기/안정성 | 소스 반복 생성/삭제, 파일 교체, 장면 전환, 장시간 실행, device reset 시나리오 검증. | RND-03, RND-05 | crash/leak 없음, GPU 메모리 안정 | `plugins/image-source/image-source.c:image_source_destroy` / `libobs-d3d11/d3d11-subsystem.cpp:device_register_loss_callbacks` |

---

## 7. Implementation List

| Impl ID | 관련 Task ID | 구현 목표 | 진입점(실제 파일 + 심볼 후보) | 의존성 | 구현 요약 | 작업 과정(steps) | 사용된 기술 | 왜 이렇게 하는지 | Codex Task Prompt |
|---|---|---|---|---|---|---|---|---|---|
| IMP-01 | BLD-01, BLD-02 | 신규 `model3d-source` 플러그인 골격 생성 | `plugins/CMakeLists.txt:add_obs_plugin` 패턴, `plugins/image-source/CMakeLists.txt`, `plugins/image-source/image-source.c:obs_module_load` | 없음 | image-source 템플릿 기반으로 module entry/CMake/data 폴더 스캐폴딩 | 1) `plugins/model3d-source` 생성 2) CMake target/source 등록 3) `OBS_DECLARE_MODULE` + `obs_module_load` 작성 4) data/locale/effect 기본 파일 배치 5) plugins 루트에 add_obs_plugin 추가 | CMake, obs-module API | 기존 구조를 따를수록 리뷰/유지보수 비용이 낮음 | "IMP-01만 수행: image-source 패턴을 참고해 plugins/model3d-source의 CMake/모듈 엔트리/데이터 폴더를 스캐폴딩하고 빌드 연결까지 완료하라. 다른 Impl 변경 금지." |
| IMP-02 | UX-01, UX-02, UX-03, LDR-01 | 파일 선택/옵션/로케일 UI 및 설정 플로우 구현 | `libobs/obs-properties.h:obs_properties_add_path`, `plugins/image-source/image-source.c:image_source_properties` | IMP-01 | glb/gltf path 및 최소 옵션 프로퍼티 제공, locale 키 연결 | 1) properties 함수 생성 2) path filter에 `*.glb *.gltf` 설정 3) bool/list 옵션 추가 4) defaults/update에서 settings 바인딩 5) locale key 정의(en-US 우선) 6) 잘못된 경로 warning 추가 | OBS properties API, locale | 사용자 입력/설정 경로를 먼저 고정해야 로더/렌더 구현이 단순해짐 | "IMP-02만 수행: model3d source에 glb/gltf 파일 선택 프로퍼티와 최소 렌더 옵션(예: flipY/alpha mode)을 추가하고 locale 키를 연결하라." |
| IMP-03 | RND-01, RND-02 | source 기본 수명주기 + 렌더 함수 뼈대 구현 | `libobs/obs-source.h:struct obs_source_info`, `libobs/obs-module.c:obs_register_source_s`, `libobs/graphics/graphics.c:gs_effect_create_from_file` | IMP-01, IMP-02 | create/destroy/update/get_size/video_tick/video_render 기본 루프 구축 | 1) source context struct 정의 2) obs_source_info 채우기 3) effect 로딩/해제 루틴 구현 4) width/height 정책 정의(모델 AABB 또는 고정 기준) 5) 빈 draw(테스트 삼각형)로 smoke 확인 | obs_source_info, gs_effect, gs_draw | 초기엔 렌더 계약을 먼저 안정화해야 후속 로더 결합 시 디버깅이 쉽다 | "IMP-03만 수행: model3d source의 obs_source_info/생명주기/기본 video_render 루프를 구현하고 effect 로딩까지 완료하라." |
| IMP-04 | LDR-01, LDR-02, LDR-04 | glTF/GLB CPU 로더 구현(MVP subset) | 신규 파일(예: `plugins/model3d-source/model3d-loader.*`), 참고: `plugins/text-freetype2/obs-convenience.c` VB 데이터 패턴 | IMP-03, BLD-03 | parser로 메시+UV+baseColor 텍스처만 파싱/디코드해 CPU payload 생성 | 1) parser 래퍼 작성 2) bufferView/accessor 해석 3) primitive triangulation 확인 4) 이미지 소스(uri/bufferView) 디코드 5) CPU payload 구조 정의 6) 오류코드/로그 체계화 | cgltf(또는 동급), 메모리 파서, 이미지 디코드 | MVP 범위를 강제 제한해 안정성을 먼저 확보 | "IMP-04만 수행: glTF/GLB에서 positions/uv0/indices/baseColor 텍스처를 추출하는 CPU 로더를 구현하고 실패 시 안전하게 에러를 반환하라." |
| IMP-05 | LDR-03, RND-03 | 비동기 로딩 + 그래픽스 스레드 업로드 브릿지 | `plugins/image-source/image-source.c:file_decoded/texture_loaded`, `libobs/obs.c:obs_enter_graphics` | IMP-04 | worker thread parse 결과를 render tick에서 업로드하도록 상태기계 구현 | 1) worker job queue/취소 토큰 구현 2) pending payload mutex 보호 3) video_tick에서 payload polling 4) `obs_enter_graphics`로 VB/IB/Texture 생성 5) old/new 리소스 스왑 6) destroy 시 thread join + graphics safe free | threading, atomics, obs_enter_graphics | 렌더 스레드 stall 방지와 안전한 GPU lifetime 양립 | "IMP-05만 수행: model3d source에 비동기 로더와 render-thread GPU 업로드 상태기계를 추가하라. GPU 생성/해제는 obs_enter_graphics 구간에서만 수행하라." |
| IMP-06 | RND-04, RND-05, TST-02 | 색공간 정확화 + D3D11 검증 훅 | `plugins/image-source/image-source.c:image_source_render`, `libobs-d3d11/d3d11-texture2d.cpp:InitResourceView`, `libobs-d3d11/d3d11-subsystem.cpp:device_register_loss_callbacks` | IMP-05 | sRGB 텍스처 샘플링/알파 블렌드 규칙 고정, D3D11 리셋 대응 최소 설계 | 1) effect 파라미터 `image`는 sRGB setter 사용 2) framebuffer_srgb push/pop 3) OBS_SOURCE_SRGB flag 검토 4) D3D11에서 linear/sRGB 출력 비교 캡처 체크 5) device-loss 발생 시 재업로드 트리거 경로 마련 | gs_effect_set_texture_srgb, D3D11 SRV, device-loss callback | 색 틀어짐/알파 오염은 릴리즈 결함으로 직결되어 MVP에서 선제 차단 필요 | "IMP-06만 수행: model3d source의 sRGB/linear 처리와 알파 블렌딩을 정합시키고 D3D11 기준 검증 포인트를 코드에 반영하라." |
| IMP-07 | TST-01, TST-03, TST-04 | 테스트/검증 시나리오 자동화 및 문서화 | `test/CMakeLists.txt`, `test/test-input/test-input.c` 패턴 | IMP-06 | 최소 smoke + scene transform + 장시간 안정성 체크 절차 정리(가능 범위 자동화) | 1) test-input 유사 테스트 모듈/스크립트 설계 2) 소스 생성/삭제 반복 테스트 3) scene transform regression 케이스 4) D3D11 전용 수동 체크리스트 문서화 5) CI job 후보 정의 | cmake test harness, smoke/regression | 기능 추가 후 회귀를 빠르게 잡기 위한 최소 안전망 | "IMP-07만 수행: model3d source의 smoke/scene transform/수명주기 테스트 계획을 test 하네스에 맞게 정리하고 실행 스크립트 초안을 작성하라." |

---

## 8. Risk Register + Mitigations

| 리스크 | 영향 | 가능성 | 대응 방안(Mitigation) | 관련 Task/Impl |
|---|---|---|---|---|
| D3D11 색공간 처리 오류(sRGB/linear 혼선) | 색 왜곡/알파 테두리 | 중 | `gs_effect_set_texture_srgb` 강제, framebuffer_srgb 토글 표준화, 레퍼런스 스냅샷 테스트 | RND-04, TST-02, IMP-06 |
| 렌더 스레드 블로킹(대형 모델 파싱) | 프레임 드랍/UI 멈춤 | 높음 | worker parse + render-thread upload 분리, 파일 변경 debounce | LDR-03, IMP-05 |
| GPU 리소스 수명주기 결함 | 크래시/메모리 누수 | 중 | 생성/파괴를 graphics context로 제한, destroy/join 순서 고정 | RND-03, TST-04, IMP-05 |
| 디바이스 손실/재초기화 미대응 | 출력 중단/검은 화면 | 중 | MVP는 재업로드 트리거 + null-safe draw, Phase 2에서 loss callback 정식 연동 | RND-05, IMP-06 |
| 라이선스 비호환 3rdparty 도입 | 배포 불가 | 중 | GPL-2.0 호환성 검토 체크리스트, NOTICE/SPDX 명시 | BLD-03, IMP-04 |
| glTF 기능 범위 과확장 | 일정 지연 | 높음 | MVP 스키마 고정(메시+baseColor), 나머지 Phase 2 백로그로 분리 | 전체 |

---

## 9. Follow-up / Phase 2 backlog

### Backend 확장
- OpenGL 경로 정합성 검증(`libobs-opengl`) 및 셰이더 분기.
- Metal 경로 정합성 검증(`libobs-metal`) 및 macOS 테스트 매트릭스 추가.

### 기능 확장
- PBR(Metallic-Roughness), normal/occlusion/emissive 맵.
- 스키닝/애니메이션/모프 타깃.
- 다중 머티리얼/서브메시 배치 최적화.
- IBL/환경광/간단한 그림자.
- 카메라/라이트 노드 해석, gizmo 연동(편집 UX).

### 성능/운영
- mesh/texture 캐시(동일 파일 재사용).
- 백그라운드 프리페치, async cancel 고도화.
- GPU 메모리 budget 추적 및 진단 오버레이.

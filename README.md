# UE8_TonemapMasters

Unreal Engine 5 프로젝트. `Plugins/TonemapMaster` 플러그인이 엔진 기본 톤매퍼를 AgX 기반 커스텀 톤매퍼로 교체한다.

### 특징

- **엔진 셰이더 파일을 전혀 수정/패치하지 않는다.** 필요한 셰이더(`PostProcessCombineLUTs`, `PostProcessTonemap` 등)는 플러그인 `Shaders/` 폴터로 복사·포팅해 자체 가상 디렉터리(`/Plugin/TonemapMaster`)에서 컴파일하고, 엔진 측 `.usf` 파일이나 렌더러 소스에는 일절 손대지 않는다. 엔진 업데이트 시 플러그인만 유지보수하면 되고, 소스 빌드 엔진이 없는 런처 엔진 환경에서도 동작한다.
- 교체는 엔진 공식 확장 지점(`EPostProcessingPass::ReplacingTonemapper` + SceneViewExtension)만 사용하므로 CVar 하나로 언제든 순정 톤매퍼로 복귀 가능하다.

## TonemapMaster 아키텍처

### 모듈 구성

- **`TonemapMaster`** (Runtime, `PostConfigInit` 페이즈) — 렌더링 파이프라인 개입 로직. 글로벌 셰이더 컴파일 전에 `/Plugin/TonemapMaster` 가상 셰이더 디렉터리 매핑이 필요해 이 로딩 페이즈를 사용한다 (`TonemapMasterModule.cpp`).
- **`TonemapMasterEditor`** (Editor) — Slate 기반 설정 패널. LevelEditor 메인 메뉴에 "Mazeline" 서브메뉴를 삽입해 tonemap master 탭 `STonemapMasterPanel`을 연다.

### 파이프라인 삽입 지점

핵심은 `FTonemapMasterSceneViewExtension::SubscribeToPostProcessingPass` (`TonemapMasterSceneViewExtension.cpp:182`). **`EPostProcessingPass::ReplacingTonemapper`**에 구독하면 엔진이 자체 톤맵 패스를 통째로 건어너뛰는 공식 훅이다. 구독 조건은 `r.TonemapMaster.Enable` 활성 + SDR 출력인 경우뿐이며, HDR 출력(ST2084/scRGB)에서는 엔진 ACES에 맡긴다.

### 2-패스 구조 (v2)

**Pass A — 3D 그레이딩 LUT 빌더** (`TonemapMasterCombineLUTs.usf`, 컴퓨트 셰이더)

- 엔진의 `PostProcessCombineLUTs`를 포팅. ReplacingTonemapper를 쓰면 엔진이 자체 CombineLUT 패스도 건어너뛰므로 플러그인이 직접 만든다.
- 화이트밸런스, 컬러 그레이딩, ExpandGamut, legacy LUT, AgX 톤 커브, 출력 디바이스 EOTF 인코딩을 32³ 볼륨 LUT에 베이크한다.
- 캐싱: LUT에 영향을 주는 모든 입력을 `FTonemapMasterLUTKey`로 모아 CityHash64 해시, 달라질 때만 리빌드 (`TonemapMasterCombineLUTs.cpp:178`). 뷰 키는 제외해 다중 뷰가 LUT를 공유한다.

**Pass B — 픽셀당 톤맵** (`TonemapMasterPS.usf`)

- 엔진 `PostProcessTonemap.usf` 포팅: scene color + bloom 합성 → eye-adaptation 노출 → 비네트 → Pass A의 3D LUT 샘플(LinToLog 셰이퍼 + scale/offset) → 디더링.

### 에디터 패널 ↔ 런타임 연동

패널은 상태를 들고 있지 않고 CVar를 읽고/쓰는 방식이라 즉시 반영된다.

- `r.TonemapMaster.Enable` (기본 1) — 0이면 구독이 꺼져 엔진 filmic 톤매퍼로 복귀
- `r.TonemapMaster.Look` (Default/Golden/Punchy, 기본 Punchy)
- `r.TonemapMaster.ContrastMode` (LUT/다항식 근사, 기본 LUT)

Look/Contrast 변경은 LUT 해시 키에 포함되어 Pass A가 자동 리빌드된다.

## AgX 구현

AgX는 엔진 그레이딩 체인에서 `FilmToneMap()` 호출 위치에 정확히 대처리되며, 나머지 그레이딩 수학은 순정 UE 그대로다. 엔진이 AP1 working space에서 동작하므로 AgX 전후로 `AP1↔sRGB` primary-only 변환(색적응 없음 — 비선형 곡선에 화이트포인트 시프트가 베이크되는 것 방지)을 삽입한다.

수학 단계는 전부 `ApplyAgXLinear` (`TonemapMasterCombineLUTs.usf:336`) 안에서 Pass A LUT에 베이크된다.

1. **Inset 행렬** — BT.709-native `agx_mat` 곱으로 gamut compression. UE 5.7 레퍼런스와 맞추기 위해 음수를 clamp하지 않는다.
2. **Log2 인코딩** — `clamp(log2(x), -12.47393, 4.026069)` 후 [0,1] 정규화 (OCIO lg2 allocation 값 그대로).
3. **Contrast 곡선** — 두 모드:
   - LUT 모드(기본): `AgXContrastLUT.h`의 4096-엔트리 사전 계산 배열(sobotka `AgX_Default_Contrast.spi1d` 이식)을 최초 1회 1×4096 `PF_R32_FLOAT` 텍스처로 업로드, 텍셀 중심 어드레싱으로 bilinear 샘플 — OCIO와 bit-exact.
   - 근사 모드: Wrensch minimal AgX 6차 다항식 (최대 오차 ~5.8e-3).
4. **Look (ASC CDL)** — `pow(max(val*slope+offset, 0), power)` + CDL 입력 luma 기준 채도. Default/Golden/Punchy 3종.
5. **Outset 행렬** — inset의 역행렬 (디스플레이 EOTF가 아님).
6. **EOTF 프리-인코딩 `pow(x, 2.2)`** — 하류 엔진 출력 디바이스 경로가 이미 EOTF를 적용하므로 이중 인코딩(washed out) 방지용 트릭.

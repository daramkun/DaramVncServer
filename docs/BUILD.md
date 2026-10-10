# 빌드 매뉴얼

DaramVncServer를 소스에서 빌드하고 검증하는 방법입니다. 서버를 설치·운영하는 방법은 [사용법 매뉴얼](USAGE.md)을 보세요.

## 1. 준비물

| 항목 | 요구 사항 |
|---|---|
| OS | Windows 10 이상 (x64). Desktop Duplication(DXGI)과 Media Foundation을 씁니다 |
| 컴파일러 | Visual Studio 2022 이상 또는 Build Tools, **"C++를 사용한 데스크톱 개발"** 워크로드 (MSVC x64, Windows SDK) |
| CMake | 3.25 이상 (Visual Studio에 포함된 것도 가능) |
| Ninja | 빌드 생성기 (Visual Studio에 포함) |
| vcpkg | **`C:\vcpkg`** 에 설치되어 있어야 합니다 (`CMakePresets.json`이 이 경로를 가리킵니다) |
| Python 3 | 검증 스크립트(`scripts\*.py`)를 돌릴 때만 필요 |

vcpkg가 없다면:

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

다른 경로에 두려면 `CMakePresets.json`의 `CMAKE_TOOLCHAIN_FILE`을 고치세요.

## 2. 의존 라이브러리

`vcpkg.json`(매니페스트 모드)에 선언되어 있어 **첫 구성 때 vcpkg가 자동으로 내려받아 빌드합니다.** 따로 설치할 필요는 없습니다.

| 라이브러리 | 용도 |
|---|---|
| zlib | 표준 zlib API (CMake 의존성 해결용) |
| zlib-ng | Tight/ZRLE의 zlib 스트림 압축 (SIMD) |
| libjpeg-turbo | 사진 영역 JPEG 인코딩 |
| openssl | TLS(VeNCrypt), 인증서 생성, PBKDF2 |

트리플렛은 `x64-windows-static`이고 CRT도 정적(`/MT`)이라서 산출물은 별도 DLL 없이 실행됩니다. 첫 빌드는 OpenSSL 때문에 수 분에서 십수 분이 걸리고, 이후에는 캐시가 쓰입니다.

## 3. 빌드

### 한 번에 (권장)

```bat
scripts\build.bat
```

`vswhere`로 Visual Studio를 찾아 `vcvars64`를 호출한 뒤 구성(`cmake --preset release`)과 빌드(`cmake --build build`)를 합니다. **개발자 명령 프롬프트가 필요 없고** 일반 `cmd`나 PowerShell에서 실행하면 됩니다. 성공하면 마지막 줄에 `Build finished: <경로>\build`가 나옵니다.

### 직접

개발자 명령 프롬프트(또는 `vcvars64.bat`를 호출한 셸)에서:

```bat
cmake --preset release
cmake --build build
```

## 4. 산출물 (`build\`)

| 파일 | 설명 |
|---|---|
| `daramvnc.exe` | **서버 본체.** 배포하는 것은 이 파일 하나입니다 |
| `daramvnc_tests.exe` | 인코더·인증·픽셀 포맷 단위 테스트 |
| `daramvnc_client.exe` | 통합 테스트·벤치마크용 RFB 클라이언트 (참조 디코더 포함) |
| `animwin.exe` | 부하 측정용 애니메이션 창 |
| `inputsink.exe` | 입력 주입 테스트용 저수준 훅 |
| `bench_encode.exe`, `bench_jpeg.exe` | 인코더 단독 벤치마크 |
| `clip_poke.exe` | 클립보드 테스트 보조 도구 |

배포용 폴더에는 `daramvnc.exe`만 복사하면 됩니다. 설정 파일(`daramvnc.conf`), 인증서(`server.crt`/`server.key`), 로그(`daramvnc.log`)는 **실행 파일과 같은 폴더**에 만들어집니다.

## 5. 검증

```bat
build\daramvnc_tests.exe                    :: 단위 테스트, 마지막 줄 "all tests passed"
python scripts\regression.py --skip-input   :: 전체 회귀 검증 (약 40초)
```

`scripts\` 아래 스크립트는 반드시 `python scripts\x.py`로 실행하세요. `python -I`는 같은 폴더의 `common.py`를 찾지 못합니다.

| 명령 | 내용 |
|---|---|
| `python scripts\regression.py` | 단위 테스트, Tight/ZRLE/Raw 디코딩을 실제 스크린샷과 비교, 영역 요청, 8비트 컬러맵, 16비트, 동시 접속 3개, IPv4/IPv6 바인딩, TLS 로그인, 퍼징, 설정 창(GUI), 키보드/마우스 주입. 항목마다 PASS/FAIL/SKIP을 출력하고 실패가 있으면 종료 코드 1 |
| `--skip-input` | 시스템 전체 입력 훅을 거는 입력 주입 테스트(약 35초)를 생략 |
| `--animation` | 애니메이션 창이 화면에 뜨는 검사를 추가: H.264, 느린 회선에서 JPEG 품질 하락·복구, RTT 200 ms 처리량, 멈춘 클라이언트 |
| `--only unit,decoders,...` | 일부 검사만 실행 |
| `--build DIR` | `build` 외의 폴더를 검사 |
| `python scripts\stall_test.py` | 읽지 않는 클라이언트가 다른 클라이언트의 알림을 지연시키지 않는지 확인 |
| `python scripts\bench_cpu.py --mode both` | 고정 부하에서 서버 CPU 사용량 측정 (`photo` / `text` / `both` / `desktop`) |
| `python scripts\fuzz.py <포트>` | 잘못된 RFB 세션을 보낸 뒤 서버가 살아 있는지 확인 (서버를 `--view-only`로 먼저 띄울 것) |

**주의**

- 검증 스크립트는 자체 서버를 `--view-only`로 따로 띄우고 끝나면 종료하므로 실행 중인 서버에 영향을 주지 않습니다. 단, **포트 5931~5974 범위를 씁니다.**
- 애니메이션 검사는 테스트 동안 화면에 창이 뜹니다.
- 입력 주입 테스트는 약 35초 동안 시스템 전체 입력 훅을 겁니다. 주입된 이벤트만 삼키므로 직접 치는 키와 마우스는 그대로 통과합니다.
- 화면과 비교하는 검사는 캡처 도중에도 화면이 바뀌므로 약간의 차이(기본 3 %)를 허용합니다. 화면에서 동영상이 재생되는 중이면 실패할 수 있습니다.

## 6. 버전과 자동 릴리스

버전은 `<주>.<부>.<빌드 번호>` 형식이고 `daramvnc.exe version`으로 확인합니다(서버를 시작할 때 로그 첫 줄에도 나옵니다). 주·부 버전은 `CMakeLists.txt`의 `DARAM_VERSION_BASE`(현재 `0.1`)에서 바꾸고, 빌드 번호는 환경 변수 `DARAM_BUILD_NUMBER`로 넣습니다. 로컬 빌드는 번호가 `0`입니다.

`main` 브랜치에 푸시할 때마다 GitHub Actions(`.github/workflows/release.yml`)가 다음을 합니다.

1. `scripts\build.bat`로 빌드합니다. 빌드 번호는 워크플로 실행 번호(`github.run_number`)이며 푸시할 때마다 1씩 오르고 줄어들지 않습니다.
2. `daramvnc_tests.exe`를 실행합니다. 실패하면 릴리스를 만들지 않습니다.
3. `DaramVncServer-<버전>-win-x64.zip`(서버, README, 매뉴얼)과 SHA-256 파일을 만듭니다.
4. 태그 `v<버전>`으로 GitHub Release를 만들고 위 파일을 올립니다.

첫 실행은 vcpkg가 OpenSSL 등을 빌드해서 오래 걸리고(수십 분일 수 있음), 이후에는 캐시를 씁니다. 수동으로 돌리려면 Actions 탭의 "Build and release"에서 Run workflow를 누르세요.

## 7. 문제 해결

| 증상 | 원인과 해결 |
|---|---|
| `vswhere.exe not found` | Visual Studio(또는 Build Tools)가 설치되어 있지 않습니다. "C++를 사용한 데스크톱 개발" 워크로드를 설치하세요 |
| `No Visual Studio with the MSVC x64 toolset was found` | 워크로드에 MSVC x64 도구 집합이 빠져 있습니다 |
| `Could not find toolchain file: C:/vcpkg/...` | vcpkg가 `C:\vcpkg`에 없습니다. 설치하거나 `CMakePresets.json`을 수정하세요 |
| 첫 구성이 매우 오래 걸림 | 정상입니다. vcpkg가 OpenSSL 등을 소스에서 빌드합니다 |
| 링크 오류 `LNK2038 ... MT_StaticRelease / MD_DynamicRelease` | vcpkg 트리플렛과 CRT 설정이 어긋났습니다. 프리셋을 쓰고(`x64-windows-static` + `/MT`) 수동 구성 시 같은 값을 지정하세요 |
| 소스 수정 후 새 `.cpp`를 인식 못 함 | `CMakeLists.txt`가 `CONFIGURE_DEPENDS`로 glob하므로 다시 `cmake --build build`만 하면 됩니다 |
| 재빌드 시 `daramvnc.exe`에 쓸 수 없음 | 서버가 실행 중입니다. 종료(`Enter` 또는 `daramvnc stop`) 후 다시 빌드하세요 |
| 검증 스크립트가 `common`을 못 찾음 | `python -I`로 실행했습니다. `-I` 없이 `python scripts\x.py`로 실행하세요 |

## 8. 소스 구조

```
src/
  main.cpp          명령행 진입점 (run / passwd / vncpasswd / install / ...)
  rfb/              RFB 프로토콜: Server, Connection(핸드셰이크, 갱신, 클립보드, 영상)
  capture/          DXGI Desktop Duplication, GDI 폴백, 멀티 모니터, CaptureHub(공유 캡처)
  encode/           Tight, ZRLE, Raw, H.264, 색 집합, 품질 제어
  security/         VeNCrypt/TLS, PBKDF2 비밀번호 해시, 인증서 생성
  input/            SendInput 주입, 클립보드
  service/          Windows 서비스와 세션 에이전트
  net/ util/ config/  전송 계층, 로그, 설정
tests/              단위 테스트(참조 디코더 포함), 입력 주입 테스트
tools/              클라이언트, 벤치마크, 부하 창
scripts/            빌드와 검증 스크립트
```

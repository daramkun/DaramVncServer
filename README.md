# DaramVncServer

Windows용 고속 VNC(RFB) 서버. 업무용(문서·IDE·브라우저) 화면을 TigerVNC 같은 **기존 표준 뷰어**로 RDP에 가까운 반응성으로 보는 것이 목표입니다. C++20 / MSVC.

## 빌드

필요: Visual Studio 2022+ (MSVC), CMake, Ninja, vcpkg(`C:\vcpkg`).

```bat
call "...\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset release        :: 처음 한 번 vcpkg가 zlib, libjpeg-turbo, OpenSSL을 빌드합니다
cmake --build build
build\daramvnc_tests.exe      :: 인코더/인증 단위 테스트
```

산출물: `daramvnc.exe`(서버), `daramvnc_tests.exe`, `daramvnc_client.exe`(통합 테스트/벤치마크 클라이언트), `animwin.exe`(부하용 애니메이션 창).

## 사용

```text
daramvnc run [--port N] [--bind ADDR] [--view-only] [--h264]   포그라운드 실행 (Enter로 종료)
daramvnc passwd <user>        로그인 사용자/비밀번호 설정 (PBKDF2-SHA256 해시만 저장)
daramvnc vncpasswd            VNC 비밀번호 설정 (DPAPI로 보호해 저장)
daramvnc install [--open-firewall]   서비스 설치 (관리자)    daramvnc start | stop | uninstall
```

설정은 실행 파일 옆 `daramvnc.conf`(`bind`, `port`, `user`, `password_hash`, `view_only`, `allow_legacy_vnc_auth`, `log` …).

- 인증 정보가 없으면 **127.0.0.1에서만** 수신하고 `bind`를 바꾸면 시작을 거부합니다.
- 원격 접속: `daramvnc passwd me` 후 `bind=0.0.0.0` 설정. 뷰어에서 **VeNCrypt(X509Plain, TLS)** 로 접속합니다. 첫 실행 시 자체 서명 인증서(`server.crt/key`)를 만듭니다.
- 서비스 모드는 활성 콘솔 세션에 SYSTEM 권한 에이전트를 띄워 로그인 화면·UAC(보안 데스크톱)를 따라갑니다.

## 구현된 표준 / 확장

| 영역 | 내용 |
|---|---|
| 프로토콜 | RFB 3.8 (3.7 클라이언트도 허용), PixelFormat 협상(8/16/32bpp true-color) |
| 인코딩 | Tight(fill / palette / zlib / **JPEG**), ZRLE, CopyRect, Raw, **H.264(선택)** |
| 의사 인코딩 | Cursor, DesktopSize, **ExtendedDesktopSize**(모니터 배치), Fence, **ContinuousUpdates**, Extended Clipboard(UTF-8), JPEG 품질 수준, QEMU 확장 키, SetDesktopSize, SetColourMapEntries(8bpp) |
| 보안 | VeNCrypt X509Plain / X509Vnc (TLS 1.2+), 선택적 레거시 VNC 인증, 실패 지연·잠금 |
| 입력 | 키보드(레이아웃 인식 VK + 유니코드), 마우스·휠, Ctrl+Alt+Del(서비스에서), 접속 종료 시 눌린 키 해제 |

## 속도 설계

- **DXGI Desktop Duplication**: dirty/move rect만 GPU에서 읽음. 변화 없으면 블로킹 대기(유휴 CPU ≈ 0). 스크롤·창 이동은 CopyRect.
- **타일 분류**(64×64): 단색 → fill, ≤256색 → palette+zlib, 그 외 사진류 → libjpeg-turbo JPEG(병렬 압축). 화면이 멈추면 JPEG 영역을 **무손실로 재전송**.
- **ContinuousUpdates + Fence**: 요청-응답 왕복 제거. Fence 응답 간격으로 대역폭을 추정해 JPEG 품질·zlib 레벨을 자동 조절하고, 전송 중인 데이터량을 `대역폭×~80ms`로 제한해 지연이 쌓이지 않게 함.
- 수신(입력)·갱신(캡처/인코딩/전송) 스레드 분리, `TCP_NODELAY`, 작은 송신 버퍼.

측정(1920×1200, 루프백, 이 개발 PC, `tools/bench_encode.cpp`와 `animwin.exe` 애니메이션 부하):

| 항목 | 최적화 전 | 최적화 후 |
|---|---|---|
| 텍스트+사진 애니메이션: 서버 CPU | 코어의 57.6% | **23~25%** |
| 같은 부하의 전송량 (업데이트 70/s) | 102 Mbit/s | **54 Mbit/s** |
| 일반 데스크톱 사용 시 서버 CPU | 5.3% | **1.2~1.6%** (화면 변화량이 달라 직접 비교는 어려움) |
| 영상형 1280×720 프레임 JPEG 인코드 | 4.18 ms | **1.29 ms** |
| 실제 화면 1프레임 Tight 무손실 인코드(zlib 레벨 1) | 14.7 ms | **6.6 ms** |
| 6MB/s 제한 링크에서 같은 부하 | 52 업데이트/s | **72 업데이트/s** |
| 애니메이션 부하에서 업데이트당 서버 CPU / 전송 MB당 CPU | 8.4 ms / 47 ms | **약 3.6 ms / 약 32 ms** (실행마다 ±20% 흔들림) |

적용한 최적화: 영속 스레드 풀과 JPEG 출력 버퍼 재사용, 사진 영역 세로 병합(JPEG 헤더·호출 감소), 사진형 JPEG 크로마 4:2:0, zlib-ng(SIMD) 사용, 색 집합 마지막 색 캐시, 업데이트 메시지 버퍼 재사용, 팔레트 인덱스 빌드를 직접 쓰기로 변경, 실제로 바뀐 픽셀만 전달(동일 내용 재그리기 제거, GPU 읽기와 같은 패스에서 비교), 허브가 캡처러 버퍼를 락으로 공유(프레임당 전체 복사 한 번 제거), 유휴 대기 시간 연장, 송신 버퍼 자동 조정 허용(고지연 회선 처리량 제한 해제).

## 이전 "알려진 한계"의 처리 결과

| 한계 | 처리 |
|---|---|
| 요청 영역 무시 | **해결.** 클라이언트가 요청한(또는 ContinuousUpdates로 지정한) 영역만 보내고, 영역 밖 변경은 클라이언트별로 쌓아 두었다가 영역이 넓어지면 보냅니다. |
| 클라이언트마다 별도 캡처 | **해결.** `CaptureHub`가 캡처 스레드 하나를 공유하고 클라이언트별 손상 영역을 관리합니다. 접속자가 없으면 캡처를 멈춥니다. |
| SetDesktopSize 거부 | **부분 해결.** 디스플레이가 지원하는 정확한 모드면 `ChangeDisplaySettings`로 실제 해상도를 바꾸고(마지막 클라이언트가 끊기면 원복), 그렇지 않으면 "잘못된 배치(3)"로 거절합니다. 임의 크기는 가상 디스플레이 드라이버가 필요해서 불가능합니다. 모니터가 여러 개면 거부합니다. |
| 8bpp 컬러맵 | **해결.** RGB332 팔레트를 `SetColourMapEntries`로 보냅니다. |
| QEMU 확장 키 | **해결(혼합 방식).** 글자 키는 keysym 경로(클라이언트 IME 호환), Ctrl/Alt 단축키와 비문자 키는 스캔코드로 보냅니다. |
| H.264 | **구현, 기본 꺼짐.** `h264=1`(또는 `--h264`)을 켜고 클라이언트가 Open H.264(인코딩 50)를 광고할 때만, 영상처럼 연속 변경되는 큰 영역에 씁니다. 멈추면 무손실로 다시 보냅니다. |

## H.264 사용

Media Foundation 소프트웨어 인코더(베이스라인 프로필, B-프레임 없음)를 씁니다. 영역마다 하나의 컨텍스트를 두고 첫 프레임에 `ResetContext`와 I-프레임을 보냅니다. 같은 노이즈 사진 애니메이션에서 Tight(JPEG) 177 Mbit/s 대비 18.5 Mbit/s였습니다. **어떤 뷰어가 인코딩 50을 실제로 디코드하는지는 확인하지 못했기 때문에 기본은 꺼 두었습니다.** 사용하는 뷰어가 지원하면 `daramvnc run --h264`로 켜 보세요(인코더 오류 시 자동으로 Tight로 되돌아갑니다).

## 검증 현황

검증됨: 인코더·인증·픽셀 포맷 단위 테스트, 독립 참조 디코더로 Tight/ZRLE/Raw/컬러맵 라이브 디코드(스크린샷 비교), 영역 요청·3클라이언트 동시 접속, TLS+로그인, 비정상 입력 퍼징, 처리량/적응 벤치마크, H.264 인코드→Windows MF 디코드 라운드트립(PSNR 37 dB)과 라이브 영상 영역 전송, SetDesktopSize 거절/무변경 경로.

**검증하지 못한 것 (확인 필요):**
- **실제 뷰어(TigerVNC, RealVNC, UltraVNC)와의 호환성** — 서명 확인이 불가능한 바이너리는 실행하지 않았습니다. H.264는 특히 뷰어 지원 여부 확인이 필요합니다.
- **서비스 설치/에이전트, 잠금 화면·UAC(보안 데스크톱) 캡처** — 관리자 권한이 필요해 설치해 보지 않았습니다.
- **멀티모니터**(모니터별 duplication 합성) — 이 PC는 모니터가 1개입니다.
- **실제 해상도 전환**(사용자의 창 배치가 바뀔 수 있어 실행하지 않음), 클립보드 공유, 키보드/마우스 주입(`--view-only`로만 테스트).

**남은 제약:** 컬러맵은 고정 RGB332 팔레트(적응형 아님) · SetDesktopSize는 디스플레이가 제공하는 정확한 해상도만 · H.264 디코더가 재정렬 정보를 요구하면 클라이언트에서 한 프레임 지연이 생길 수 있음(Windows MF 디코더에서 관찰) · 영상 판정은 휴리스틱(연속·대형·사진형 변경)이라 일부 콘텐츠는 Tight로 남음.

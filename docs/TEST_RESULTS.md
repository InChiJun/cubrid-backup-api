# Tiered Buffer — 테스트 결과 (feature/tiered-buffer)

실제 CUBRID 11.5 환경에서 실제 `backupdb` + `restoredb`로 수행. 무결성은 매번
백업→(로그 제거 후)복원→**내용 시그니처**(`count, sum(a), sum(c), sum(len(b))`)를
백업 전 기준값과 비교해 판정(restoredb의 페이지 체크섬 검증 + 논리 내용 일치).

대상 DB: `tbstress` (100MB 볼륨, 20,000행 varchar/bigint), 백업 산출물 ~7–11MB.

---

## 1. 계층 × 소비자속도 매트릭스 (Round 1) — 9/9 PASS

느린 다운스트림(테이프)에서 각 tier가 실제로 동작하고 산출물이 무결한지.

| # | 시나리오 | 버퍼 | 소비자 지연 | 결과 |
|---|----------|------|-------------|------|
| S1 | 레거시 OFF, 빠름 (회귀) | mem=0 | 0 | 복원 sig 일치 |
| S2 | 레거시 OFF, 느림(테이프) | mem=0 | 4ms/read | 복원 sig 일치 |
| S3 | 메모리 64MB 기본, 빠름 (회귀) | mem=64MB | 0 | 복원 sig 일치 |
| S4 | **메모리 전용, 초저속** (mem WAIT) | mem=1MB | 9ms/read | 복원 sig 일치 |
| S5 | **디스크 spill**, 느림 | mem=1MB, disk=16MB | 6ms/read | 복원 sig 일치 |
| S6 | **소형 디스크 heavy spill** | mem=1MB, disk=2MB | 6ms/read | 복원 sig 일치 |
| S7 | fifo_size=8MB → 1MB clamp | mem=8MB | 2ms/read | 복원 sig 일치 |
| S8 | **cancel 중간 종료** (무행업) | mem=1MB, disk=16MB | 3ms/read | 정상 반환, 행업 없음 |
| S9 | **핸들 5회 재사용**(단일 프로세스) | mem=1MB, disk=16MB | 3ms/read | 누수/상태오염/행업 없음 |

## 2. 핵심 가치 — LOG_CS 점유가 다운스트림 속도와 분리됨 (§11.2 수용 기준)

동시 DML로 로그 구간을 만든 뒤, 서버 에러로그의 `ER_LOG_BACKUP_CS_ENTER`(-1087) ~
`_EXIT`(-1088) 간격으로 backupdb의 LOG_CS 점유시간을 실측. 소비자 지연을 sweep.

| read당 지연 | 완충 OFF 점유 | 완충 ON(64MB) 점유 | 배율 |
|-------------|---------------|--------------------|------|
| 10ms | 2,794 ms | 1,305 ms | 2.1× |
| 30ms | 7,271 ms | 1,529 ms | 4.8× |
| **60ms** | **23,125 ms** | **1,530 ms** | **15.1×** |

**OFF는 소비자가 느려질수록 LOG_CS 점유가 비례 증가(2.8s→7.3s→23s)하는 반면, ON은
평탄(~1.5s, backupdb 자체 복사시간에 bound)하다.** 즉 완충이 LOG_CS 점유를
downstream(테이프) 속도로부터 완전히 분리한다 — **느린 장비일수록 이득이 커진다**.
(20ms에서 3265ms→1200ms 별도 확인.)

## 3. 극단/장애 시나리오

| 시나리오 | 구성 | 결과 |
|----------|------|------|
| **테이프 드라이브 8초 완전정지** (중간 stall) | mem=2MB, disk=8MB, 5ms/read + 8s freeze | 완충 흡수+backpressure, 재개 후 11.5MB 복원 sig 일치 |
| **상시 WAIT** (총 완충 ≪ 백업) | mem=1MB, disk=1MB(2MB) vs 11.5MB, 30ms/read | 복원 sig 일치 |
| compress=true + 완충 | mem=64MB | 복원 sig 일치 |
| fifo_size=100 → 64KB로 상향 | mem=64MB | 복원 sig 일치 |
| 오설정: disk>0 + 경로 미존재 | — | init −1로 **graceful 거부**(크래시/행업 없음) |
| 오설정: disk>0 + mem=0 | — | init −1로 graceful 거부 |
| 오설정: mem < io_size (1KB) | — | init −1로 graceful 거부 |

## 4. 코드-레벨 검증 (실장 전)

- **컴파일**: out-of-tree cmake 빌드, 에러/경고 0 (M-1~M-5 각 단계).
- **파서 단위테스트**: value 정규식(경로/접미사 매칭), `set_size_value`(KB/MB/GB,
  오버플로/음수/errno) — 전부 PASS.
- **동시성(helgrind, race 0)**: 실제 pipe로 메모리 링 SPSC(4KB 링에 4MB 무손실,
  WAIT 868회), 메모리+디스크 2-tier(300B mem+1000B disk, 실제 spool, 상시
  spill/wrap/WAIT) — data race·lock-order 위반 0.

### 테스트가 잡아낸 실제 결함 (수정 완료)
2-tier 동시성 테스트가 **hang 버그**를 검출: spill 중 disk full·mem 여유 상태에서
옛 WAIT 조건(“둘 다 full”)이 대기하지 않아 `read(fifo,0)==0`→false EOF→drain
조기종료→backupdb가 full pipe에 블록→**행업**. WAIT를 *선택된 tier가 full일 때*
대기하도록 수정(commit `bf8de0f`).

---

## 요약

전 경로(레거시/메모리/디스크) × 전 속도(빠름~60ms/read~완전정지)에서 **데이터
무결성·무행업**을 실증했고, 핵심 목표인 **LOG_CS 점유의 downstream-속도 분리**를
정량으로 입증(느릴수록 최대 15×+ 단축). 오설정은 크래시 없이 graceful하게 거부.

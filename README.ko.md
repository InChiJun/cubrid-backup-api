# cubrid-backup-api

> 이 문서는 이번에 추가된 **계층형 버퍼 + 로그 구간 파서**의 설계·설정·운영을 다룹니다.
> 구조체/함수 시그니처/샘플 코드 등 **API 레퍼런스는 [README.md](README.md)** 를 참고하세요.

*FIFO로 `cubrid backupdb`를 구동해 백업 이미지를 호출자에게 스트리밍하는 C API. 프로세스 내부에 **티어드 버퍼**와 관찰형 **로그 구간 파서**를 두어, 느린 백업 소비자가 서버를 지연시키지 않도록 한다.*

**Languages:** [English](README.md) · 한국어(이 문서)

---

## 개요

`cubrid-backup-api`는 애플리케이션이 CUBRID 백업을 프로그램적으로 수행하게 해줍니다. `cubrid backupdb -D <fifo>`를 실행하고, 파이프에서 백업 스트림을 배수(drain)한 뒤, `cubrid_backup_read()`로 바이트를 호출자에게 넘깁니다(복원은 대칭 경로).

단순 파이프 배수 대비 핵심 추가 기능은, 전용 배수 스레드가 채우는 **티어드 버퍼**(메모리 링 → 디스크 스풀)와, 스트림을 관찰해 *데이터* 구간과 *트랜잭션 로그* 구간을 구분하고 spill 정책을 고르는 **관찰형 로그 구간 파서**입니다. 둘 다 **Linux/POSIX 전용**입니다.

## 배경 — 해결하려는 문제

CUBRID는 백업의 **로그 구간**(아카이브 + 활성 로그)을 전역 로그 임계구역 `LOG_CS`를 잡은 채 복사합니다. 기존 API는 호출자가 `cubrid_backup_read()`를 호출할 때만 파이프를 읽어 **완충이 없었습니다**:

- 느린 downstream(테이프, 네트워크 업로드 등)이 있으면 FIFO가 가득 참.
- `backupdb`는 논블로킹 fd에 write하며, 파이프가 차면 **`LOG_CS`를 잡은 채 `EAGAIN`에서 busy-spin**.
- `LOG_CS` 장기 점유 → 커밋 로그 flush 지연 → 서버 전역 지연.

**처방(서버 미수정, 단기 완화):** 버퍼를 *API 내부*에 두어 downstream 지연이 파이프 정체로 곧바로 전이되지 않게 흡수합니다. 파이프를 상시 배수하므로 `backupdb`는 `LOG_CS`를 빠르게 통과하고, 백프레셔는 버퍼(메모리 + 디스크 예약) 전체가 소진될 때만 발생합니다.

## 동작 방식

- **배수 스레드** — 전용 스레드가 FIFO를 `poll()`하며 파이프가 주는 만큼 버퍼로 복사(단일 생산자/단일 소비자, busy-spin 없음; `eventfd`로 즉시 취소).
- **메모리 계층** — `buffer_memory_size`로 크기가 정해지는 링 버퍼. 완충은 **opt-in**입니다: 컴파일 기본값은 `0`(off — 레거시 직접 FIFO 경로)이고, `> 0`으로 설정해야(배포된 샘플 conf는 64&nbsp;MB 사용) 배수 스레드 + 티어드 버퍼가 켜집니다. 리더(`cubrid_backup_read`)가 순서대로 꺼냄.
- **디스크 계층** — `buffer_disk_limit > 0`이면 익명 스풀 파일(`O_CREAT|O_EXCL|O_CLOEXEC`로 생성 후 즉시 `unlink`, 이어서 `fallocate`)이 메모리 초과분의 예약 공간을 제공. 순서는 항상 `[메모리(older) | 디스크(newer)]`로 보존.
- **`F_SETPIPE_SZ`** — 파이프를 확장(`[64KB, 1MB]`로 clamp)해 `backupdb`의 한 write 청크(~1&nbsp;MiB)가 한 번에 담기게 함.
- **관찰형 로그 구간 파서** — 스트림을 *엿보아(peek)* DB 데이터 볼륨이 끝나고 트랜잭션 로그가 시작되는 경계를 감지합니다. **데이터는 절대 변형하지 않으며**, 유일한 출력은 spill 정책을 고르는 `log_phase` 플래그뿐입니다. **LZ4**와 **비압축(NONE)** 포맷을 해석하며, 미지원 헤더(예: ZLIB)·미지의 포맷 버전·자가점검 실패 시 **스스로 비활성화하고 데이터를 그대로 통과**시킵니다.
- **2-모드 예약 스풀** — 파서가 켜지면 디스크 예약은 (`LOG_CS`가 중요한) *로그* 구간을 위해 아껴두고, 경계 전에는 데이터 페이지를 메모리에 유지합니다. 파서가 꺼지면 단일 모드: 스풀은 순수 메모리 오버플로로만 사용됩니다.

> 파서는 **관찰 전용**입니다 — 파서 on/off와 무관하게 생성되는 백업 이미지는 바이트 단위로 동일합니다. 로그 구간 감지는 스트림 순서에 기댄 best-effort 최적화 신호이지 API 계약이 아닙니다.

## 내부 구조 — 동시성 계약

버퍼는 단일 생산자(배수 스레드) / 단일 소비자(`cubrid_backup_read`) 설계입니다. 여기가 틀리면 서버가 hang하므로 계약을 명시합니다:

- **락.** `buf_lock`이 유일한 버퍼 락 — 링 인덱스, `producer_eof` / `buf_error` / `stop`, 조건변수 2개(`not_empty`, `not_full`)를 보호합니다. `backup_mutex`는 소비자 직렬화와 teardown 보호. 락 순서는 항상 `backup_mutex → buf_lock`이며, **`cond_wait` 중에는 `backup_mutex`를 잡지 않습니다.**
- **배수 루프.** 배수 스레드가 FIFO와 취소용 `eventfd`를 `poll()`(타임아웃 폴링·busy-spin 없음), 연속 여유 공간을 예약(링 wrap은 2회 read로 분할), 락 **밖**에서 링으로 `read()`(SPSC, 복사 1회), 이후 `buf_lock` 하에서 바이트 수를 반영하고 `not_empty` 신호.
- **취소 / teardown (deadlock-free).** `cubrid_backup_end`는 **먼저** `buf_lock`만으로 신호 — `stop` 설정, 깨끗한 EOF가 아니면 `buf_error` 설정(취소/절단 백업을 성공으로 보고하지 않음), 두 조건 broadcast — 하고 `eventfd`에 write, **그 다음**에야 `backup_mutex`를 잡아 배수 스레드 join·자원 해제. `cond_wait`에 걸린 소비자는 깨어나 `buf_error`를 보고 실패 반환하며 `backup_mutex`를 놓으므로 hang 없이 teardown이 진행됩니다.
- **SIGCHLD 격리.** 배수 스레드에서 `SIGCHLD`를 블록해 백업 워커의 `sigtimedwait`가 `backupdb` 자식을 거둡니다 — 아니면 스트림 종료가 영영 감지되지 않습니다.
- **EOF 분류.** `read()==0`은 `buf_lock` 하에서 판정합니다: 워커 정상 종료 → `producer_eof`; 에러 종료·취소 → `buf_error`; 아직 terminal이 아니면 짧고 유한한 `eventfd` 재확인 후 결정. 리더는 백업 종료를 정확히 1회(마지막 0-length `SUCCESS`)만 보고하며, 데이터와 동시에 보고하지 않습니다.

## 설정

설정은 `$CUBRID/conf/cubrid_backup.conf`의 `[backup]` 섹션에 둡니다. 없는 키는 아래 기본값으로 대체됩니다. 범위를 벗어난 `fifo_size`는 경고와 함께 clamp되고, 진짜 잘못된 값(예: 0이 아니면서 I/O 단위 미만인 `buffer_memory_size`, 메모리 계층 없는 디스크 계층)은 초기화 시 **즉시 거부(fail-fast)**됩니다.

| 키 | 기본값 | 의미 |
|---|---|---|
| `fifo_size` | `64KB` | 파이프 용량(`F_SETPIPE_SZ`); `[64KB, 1MB]`로 clamp. |
| `buffer_memory_size` | `0` | 메모리 링 크기. **컴파일 기본값 `0` = 완충 OFF(opt-in)** — 레거시 직접 FIFO 경로; `> 0`으로 설정해야 배수/티어드 버퍼가 켜짐(0이 아닐 때 I/O 단위 이상). 배포된 샘플 `cubrid_backup.conf`는 `64MB`로 켜 둠. |
| `buffer_disk_limit` | `0` | 디스크 스풀 예약 상한. `0` = 메모리 전용(디스크 계층 없음; 파서도 off). `buffer_memory_size > 0` 필요. |
| `buffer_disk_path` | *(빈 값)* | 스풀 파일 디렉토리. `buffer_disk_limit > 0`일 때 존재·쓰기 가능해야 함. |
| `buffer_disk_keep_spool` | `false` | 백업 후 스풀 파일을 제거하지 않고 유지. |

기존 `[backup]` 키(`remove_archive`, `sa_mode`, `no_check`, `thread_count`, `compress`, `except_active_log`, `sleep_msecs`)는 변경 없음.

예시 — 파서가 켜진 메모리 + 디스크 계층:

```ini
[backup]
compress=true
fifo_size=1MB
buffer_memory_size=64MB
buffer_disk_limit=256MB
buffer_disk_path=/var/tmp/cubrid_backup_spool
buffer_disk_keep_spool=false
```

## 공개 API

공개 헤더(`src/include/backup_api.h`, `cubrid_backup_api.h`로 설치)는 이번 작업으로 변경되지 않았습니다:

```c
int cubrid_backup_initialize (void);
int cubrid_backup_begin (CUBRID_BACKUP_INFO *backup_info, void **backup_handle);
int cubrid_backup_read  (void *backup_handle, void *buffer,
                         unsigned int buffer_size, unsigned int *data_len);
int cubrid_backup_end   (void *backup_handle);

int cubrid_restore_begin (CUBRID_RESTORE_INFO *restore_info, void **restore_handle);
int cubrid_restore_write (void *restore_handle, int backup_level,
                          void *buffer, unsigned int data_len);
int cubrid_restore_end   (void *restore_handle);

int cubrid_backup_finalize (void);
```

버퍼는 호출자에게 투명합니다: 예전과 동일하게 `cubrid_backup_read`가 백업 종료를 알릴 때까지 스트림을 읽으면 됩니다.

## 빌드

```sh
./build.sh            # 64비트 release (RelWithDebInfo)
./build.sh -m debug   # debug 빌드
```

산출물은 `build_x86_64_<mode>/_install/cubrid-backup-api/`에 생성됩니다. 빌드는 `pthread`를 명시 링크하고 `_GNU_SOURCE`로 컴파일합니다(`F_SETPIPE_SZ`·`fallocate`·`eventfd`에 필요). `build.sh`는 먼저 `git clean -ffdx`를 실행하므로 — **새 파일은 빌드에서 살아남으려면 반드시 `git add` 해야 합니다.**

## 테스트

의도적으로 분리된 2단 구조:

- **빠른 회귀 게이트** — `testcases/run_test.sh`. API와 테스트 클라이언트를 빌드하고 100&nbsp;MB `testdb`를 만든 뒤, 기능 케이스에 더해 이 기능의 *가벼운* 추가분을 실행합니다 — `parser_ut`(서버 불필요, 로그 구간 파서/링 결정적 단위테스트)와 `backup_tc05`(느린 소비자로 메모리 → spill → WAIT를 유발한 뒤 바이트 단위 복원 비교). 수 분 내 완료되며 `ALL PASSED` / `FAILED TEST SUMMARY`를 출력합니다.

- **무거운 / 실환경 스위트** — [`testcases/stress/`](testcases/stress/) (opt-in). 검증표를 만들어 낸 시나리오들: 파라미터 스윕, 에러/비정상 주입, 레벨별 정확성(~30&nbsp;GB), 150&nbsp;GB, 복원 정확도(NONE & LZ4, 5&nbsp;GB / 16.7M행), 롱 트랜잭션 개선 전/후 차등, 로그 구간 내용 일치, 강제 오경계 무결성, 커밋 지연 / `LOG_CS` 점유. 실행:

  ```sh
  CUBRID=/path/to/_install/CUBRID bash testcases/stress/run_stress.sh [quick|standard|full]
  ```

  스위트 ↔ 검증표 전체 매핑은 [`testcases/stress/README.md`](testcases/stress/README.md) 참조.


## 플랫폼 · 한계

- **Linux/POSIX 전용** — Windows 가드 없음(`F_SETPIPE_SZ`·`fallocate`·`eventfd`·unlink-on-open 사용).
- 완충은 **백업** 경로에만 적용(복원은 변경 없음).
- 이는 단기 완화책이며, 서버의 로그 복사를 `LOG_CS` 밖으로 옮기는 근본 수정은 범위 밖입니다.

## 저장소 구조

```
src/               API 구현(backup_api / backup_core / backup_manager / handle_manager) + 헤더
testcases/         run_test.sh(빠른 게이트) + tc/restore 클라이언트 + parser_ut + conf_test + 샘플 conf
testcases/stress/  opt-in 무거운 / 실환경 스위트 (run_stress.sh + s01~s11 + helpers)
build.sh           패키지 빌드 스크립트
VERSION            패키지 버전
```

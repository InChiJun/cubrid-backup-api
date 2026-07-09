# Tiered Buffer — 상세 설계 (구현 레퍼런스)

- 대상: `cubrid-backup-api` (backupdb pipe backup 연동 API)
- 브랜치: `feature/tiered-buffer`
- 분류: 단기 처방(short-term mitigation), Linux/POSIX 전용
- 상태: 구현 착수용 확정 설계. 멀티에이전트 검증(64 포인트) + 코드/빌드/서버 사실검증(prep A~F) 반영.

> 근거 문서: 원본 설계 초안(`tiered_buffer_design.md`), 검증 결과(`tiered_buffer_apply_points_verified.md`), prep 레퍼런스 `~/tbuf_prep/A~F`.

---

## 0. 배경과 문제 (사실검증 반영)

API는 `cubrid backupdb -D <fifo>`로 pipe(FIFO) 백업을 구동한다. 서버는 백업의 **로그 구간(archive+active log)**을 **LOG_CS(로그 전역 임계구역)를 잡은 채** 복사한다(`log_page_buffer.c` `logpb_backup()` `LOG_CS_ENTER`@8078 ~ `LOG_CS_EXIT`@8226). 데이터 볼륨 복사는 LOG_CS 밖.

**정정(서버 소스 확인):** backupdb의 목적지 write는 **`write()` 블로킹이 아니라 `O_WRONLY|O_NONBLOCK` fd에 대한 EAGAIN + `while(count>0)` busy-spin**이다(`file_io.c:2251`, `:9383`). 즉 파이프가 차면 backupdb는 **LOG_CS를 잡은 채 100% CPU로 스핀**한다 — 원본 설계가 말한 "blocking write 블록"보다 오히려 나쁘다. 결론(완충으로 파이프를 상시 배수하면 LOG_CS를 빠르게 통과)은 동일하며, **F_SETPIPE_SZ를 최대로 키울 이유가 강화**된다.

현재 API는 `cubrid_backup_read()` 호출 시에만 FIFO를 읽어 **완충이 없다** → downstream(업로드) 지연이 곧바로 파이프 정체 → LOG_CS 장기 점유 → commit flush(exclusive `CSECT_LOG`, `log_manager.c:10387`)까지 지연 → 서버 전역 지연.

---

## 1. 목표 · 범위 · 단계

- **목표:** API 내부에 완충 버퍼(메모리→디스크 2-tier)를 두어 downstream 지연이 backupdb 파이프 정체로 즉시 전이되지 않게 흡수. 용량 초과 시에만 backpressure.
- **비목표:** 서버측 근본 수정(로그 복사를 LOG_CS 밖으로)은 범위 밖.
- **범위:** 백업 경로만(restore 제외). Linux/POSIX 전용, **Windows 가드 없음**(코드베이스에 가드 관례 자체가 없음 — 확인됨).
- **단계 (최종 목표는 Phase 1+2 전체):**
  - **Phase 1 — 메모리 tier** (기본 활성 64MB): drain 스레드 + 메모리 링 + F_SETPIPE_SZ + 파싱/검증/수명/관측성. LOG_CS 완화 효과 대부분 확보, 리스크 최소.
  - **Phase 2 — 디스크 tier** (`buffer_disk_limit>0`): spool 파일(격리/fallocate/ENOSPC degrade/O_CLOEXEC), 멀티 DB 대응.

---

## 2. 동시성 프로토콜 (설계의 핵심 — 여기가 틀리면 hang)

### 2.1 락 / 역할
- **`buf_lock`**: 링 인덱스(mem/disk head·tail·len), `producer_eof`, `buf_error`, `stop`, cond 2개. **유일한 버퍼 락**.
- **`backup_mutex`**: 소비자 직렬화 + teardown 보호. **cond_wait 중에는 절대 보유하지 않는다** (원본 §12↔§7.2 충돌 해소).
- 락 순서: `backup_mutex → buf_lock`. reader만 둘 다(순차). drain은 `buf_lock`만. 역전 불가.

### 2.2 취소/종료 프로토콜 (deadlock 해소 — C4)
`end_backup`은 **backup_mutex를 잡기 전에** buf_lock으로 먼저 신호한다:
```
end_backup(h):
  validate_handle(h)                       // 포인터 동일성 검사
  // (1) SIGNAL FIRST — backup_mutex 없이
  lock(buf_lock)
     if backup_thread_state == RUNNING: is_cancel = true
     stop = true
     if (정상 EOF 아직 아님): buf_error = true      // cancel = 조기 EOF (truncated 성공보고 방지)
     broadcast(not_empty); broadcast(not_full)
  unlock(buf_lock)
  write(cancel_efd, 1)                      // poll 중 drain 즉시 기상
  // (2) reader가 backup_mutex 놓았으므로 이제 획득 가능
  lock(backup_mutex)
     if backup_thread started & RUNNING: pthread_join(backup_thread)  // backupdb kill→fifo close 유발
     if drain_started: pthread_join(drain_thread)                     // fifo EOF/efd로 이미 탈출
     close_fifo(h)                          // drain join 이후 (fifo_fd 사용 끝)
     [P2] close(disk_fd)                    // unlink-on-open → 공간 회수
     free(mem_buf)
     free_handle(h)                         // → finalize_backup_handle (idempotent)
  unlock(backup_mutex)
```
- reader가 `cond_wait(not_empty)`를 backup_mutex 보유한 채 들어가 있어도, (1)이 buf_lock만으로 broadcast → reader 기상 → buf_error 확인 → FAILURE 리턴하며 backup_mutex 해제 → (2)에서 획득. **hang 없음.**
- drain이 `cond_wait(not_full)`(WAIT)든 `poll()`(read 대기)든 (1)broadcast/`cancel_efd`로 양쪽 다 기상.

### 2.3 drain SIGCHLD 격리 (EOF 감지 hang 방지)
`begin_backup`에서 **drain 생성 직전** 호출 스레드 마스크에 SIGCHLD 블록 → `pthread_create(drain)` → 마스크 원복. drain은 SIGCHLD 블록 상태로 시작(생성-후-sigmask 레이스 제거). `backup_thread`의 기존 `sigtimedwait(SIGCHLD)` reap 경로는 손대지 않음(blast radius 최소).
> 이유: process-directed SIGCHLD가 drain으로 배달되면 backup_thread의 sigtimedwait가 자식을 못 거둬 `THREAD_STATE_EXIT`가 안 되고 EOF 감지가 영영 안 됨.
> **R1(범위 밖):** SIGCHLD가 reader/main으로 배달될 수 있는 기존 취약성은 별도 이슈. drain은 이를 악화시키지 않음만 보장.

### 2.4 EOF 분류 & 상태 발행
drain의 `read()==0`(진짜 EOF)일 때만 종료 판정. 자식 fifo-close가 `set_thread_state(EXIT)`(L933)보다 먼저 보일 수 있으므로:
- `read()==0` → **buf_lock 하에서** state 확인: `EXIT && !is_cancel`→`producer_eof`; `EXIT_WITH_ERROR || is_cancel`→`buf_error`; 아직 terminal 아니면 `poll(cancel_efd, 짧은 timeout)` 후 재확인(유한 재시도 상한, 초과 시 buf_error).
- `execute_backup`의 최종 `set_thread_state`도 buf_lock으로 publish(happens-before). `backup_thread_state`는 보조로 `volatile`.

---

## 3. 자료구조 (`src/include/handle_manager.h` `struct backup_handle` 추가)
```c
/* buffering control */
bool  buffering_enabled;     /* mem_cap>0 && malloc 성공. false=구 direct-FIFO 경로(fallback/rollback) */
bool  drain_started;         /* join 가드 (THREAD_STATE는 backup_thread 전용) */
bool  stop;                  /* 통합 종료 플래그 */
int   cancel_efd;            /* eventfd, poll 대상 */
/* memory ring (P1) */
char* mem_buf; size_t mem_cap, mem_len, mem_head, mem_tail;
/* disk ring (P2) */
int   disk_fd;               /* -1 초기화 (0=stdin 함정 주의) */
long long disk_cap, disk_len, disk_head, disk_tail;
bool  producer_eof, buf_error;
pthread_t       drain_thread;
pthread_mutex_t buf_lock;
pthread_cond_t  not_empty, not_full;
/* observability */
size_t hw_mem; long long hw_disk; bool spilled;
unsigned long wait_cnt; long long wait_us_total, bytes_total;
```
> 핸들은 **정적 임베드 인스턴스**(malloc 아님)라 begin마다 재사용 → 모든 필드는 `initialize_backup_handle`에서 정의값으로 리셋.

---

## 4. 알고리즘

### 4.1 drain 스레드 (eventfd + poll, 링 직접 read)
```
drain_main(h):
  pthread_sigmask(SIG_BLOCK, {SIGCHLD})
  for(;;):
    poll({fifo_fd:POLLIN, cancel_efd:POLLIN}, -1)     // 타임아웃 폴링/busy-spin 없음
    if (cancel_efd ready): classify & break
    lock(buf_lock)
      while (mem_free()==0 && disk_free()==0 && !stop): { wait_enter_log(); wait_cnt++; cond_wait(not_full) }  // Tier-3 WAIT
      if (stop): unlock; break
      dst,room = reserve_contiguous_free()            // §4.3 규칙, 미publish 영역(drain 독점)
    unlock(buf_lock)
    n = read(fifo_fd, dst, room)                      // O_NONBLOCK, 락 밖 (SPSC: 복사 1회)
    if (n<0): if EAGAIN/EWOULDBLOCK/EINTR continue; else { mark_buf_error(); break }
    if (n==0): classify_eof_under_lock(); break
    lock(buf_lock); commit_written(n); update_hw(); cond_signal(not_empty); unlock(buf_lock)
  lock(buf_lock); broadcast(not_empty); broadcast(not_full); unlock(buf_lock)
```
- `reserve_contiguous_free`: wrap 경계면 이번 read는 링 끝까지만(`room`=연속 free) → 다음 루프에서 앞부분 → **2-op 자동 분할**(메모리 링 wrap도 처리, C2 해소).

### 4.2 reader (pop) — `read_data`를 `buffering_enabled` 분기로 교체
```
pop(h,out,cap,*data_len,*is_backup_end):
  lock(buf_lock)
    while (mem_len==0 && disk_len==0 && !producer_eof && !buf_error): cond_wait(not_empty)
    if (buf_error): unlock; return FAILURE                         // cancel/조기EOF 포함
    if (empty && producer_eof): *is_backup_end=true; *data_len=0; unlock; return SUCCESS
    copied  = mem_read(out, cap)                                   // older first, 2-op wrap
    copied += disk_read(out+copied, cap-copied)                    // newer
    *data_len = copied                                            // is_backup_end 미변경
    cond_signal(not_full)
  unlock; return SUCCESS
```
- **reader 3값 계약 보존**(확인됨): 기본 not-end, 종료 시 `SUCCESS` 정확히 1회(0-length 가능), 데이터>0이면 `SUCCESS_FRAGMENTED`, 에러는 `FAILURE`. `is_backup_end`는 caller가 false init(backup_api.c L190), empty+eof에서만 true, data_len>0과 동시 true 금지.
- `read_backup_data`는 `backup_mutex` 유지(소비자 직렬화)하되 pop 내부 buf_lock은 memcpy 구간만. cond_wait은 buf_lock에서만.

### 4.3 순서보존 push 규칙 (원본 §6)
`disk_len>0 || mem_free()==0` → 디스크 기록, 아니면 메모리. 레이아웃 항상 `[mem(older)|disk(newer)]`/`[disk]`/`[mem]`. `disk_len==0`이면 mem tier 복귀.

---

## 5. 설정 파싱 / 검증 (`src/backup_manager.c`)

- **value 정규식 확장**: `([[:alnum:]]+)` → `([[:alnum:]/._-]+)` (경로 전용; `256MB`/`64KB`는 원래 매칭됨). key 그룹 불변.
  - 주의: value 무매치 라인은 **silent-skip**(else 없음) → 확장 전엔 `buffer_disk_path`가 에러 없이 사라짐.
- **`set_backup_option`에 5개 분기**, `strncasecmp` 길이 = **strlen+1**: fifo_size `10`, buffer_memory_size `19`, buffer_disk_limit `18`, buffer_disk_path `17`, buffer_disk_keep_spool `23`. (접두사 `buffer_disk_` 충돌 방지)
- **`set_size_value()`** 신규: long long, KB/MB/GB(1024진), **`errno=0` 선행**(기존 `set_int_value` 버그 복붙 금지), `value>LLONG_MAX/factor` 오버플로 가드. fifo_size는 int라 clamp 후 INT_MAX 재검사.
- **`set_path_value()`** 신규: PATH_MAX(4096) 복사. validate_dir 호출하지 않음(disk_limit==0이면 빈 경로 정상).
- **`init_default_backup_option`에 기본값 seed**: fifo=64KB, mem=64MB, disk=0, path="", keep=false. (안 하면 zero-init→mem=0=OFF, "기본 ON" 정반대)
- **검증은 `set_io_size()`(~L815) 직후** (그 전엔 `io_size==0`): mem≥io_size; disk>0→mem>0; disk>0→`validate_dir`(선존재·`X_OK`·mkdir 안 함); fifo clamp[64KB,1MB]; `statvfs` 여유<disk_limit→WARN(신규, validate_dir엔 free-space 없음).
  - `start_backup_manager` 순서: open_log_file → set_cubrid_home → make_backup_home → read_conf_file → set_io_size. conf 없으면 defaults로 SUCCESS.

---

## 6. FIFO / F_SETPIPE_SZ (`backup_core.c open_fifo` L501 직후)
`open(O_RDONLY|O_NONBLOCK)` 성공 직후 `fcntl(fifo_fd, F_SETPIPE_SZ, clamp(fifo_size,64KB,1MB))`. 실패해도 **`goto error` 안 함**(WARN+계속 — open_fifo 유일 예외), 반환값(실제 granted)을 로그. backupdb write 청크 ≈ 1MiB(`st_blksize×256`)라 **F_SETPIPE_SZ=1MB로 청크 1개 수용**. read-end에 적용해도 유효(backupdb가 write-end를 늦게 open). `_GNU_SOURCE` 필요.

---

## 7. Spool (Phase 2)
naming `<buffer_disk_path>/cubrid_bkbuf_<db>_L<lvl>_<pid>.spool`; `open(O_CREAT|O_EXCL|O_CLOEXEC,0600)` → **`==-1` 검사**(`IS_FAILURE(open)` 금지: `IS_FAILURE(x)=(x)!=0`이라 정상 fd도 참) → 즉시 `unlink`(unlink-on-open) → `fallocate(disk_limit)`. 실패 처리: **ENOSPC→degrade**(limit 하향/디스크 tier off, 백업 계속), **EOPNOTSUPP/ENOTSUP→ftruncate+lazy**(tmpfs 등), **EIO→buf_error**. `O_CLOEXEC`로 fork된 backupdb가 상속해 공간회수 보장을 깨지 않게. circular pread/pwrite 2-op(정렬 가정 금지). FIFO 경로(L448)도 pid 포함 검토(공유 backup_home 시 live FIFO unlink 충돌 방지).

---

## 8. 수명 / init / teardown (`src/handle_manager.c`)
- `buf_lock`/cond 2개: `initialize_handle_manager`(L11 옆)에서 **1회 init**, `finalize_handle_manager`(L33)에서 destroy.
- `initialize_backup_handle`(alloc+finalize **2회/사이클**): 링 필드 **리셋만**(`mem_buf=NULL, disk_fd=-1, cancel_efd=-1, len/head/tail=0, flags=false`). malloc/open 금지.
- `finalize_backup_handle`(teardown 3사이트: begin-error L1007 / end_backup L1061 / stop_handle_manager L138): drain join(가드)→close(disk_fd/cancel_efd, `>=0`만)→free(mem_buf)→**L88 re-init 전**, 후 `drain_started=false`로 idempotent(double-join UB 방지).
- `begin_backup`(backup_mutex 보유 중): open_fifo & set_backup_info 후, buffering이면 mem_buf malloc(**실패→`buffering_enabled=false` degrade**)→eventfd→[P2 spool]→SIGCHLD 마스크→create drain→`drain_started=true`. 에러 switch `case 2:` 위에 신규 케이스(fallthrough 유지).
- `set_backup_info`(L297)에서 option→handle 전파(누락 해소). `disk_fd`/`cancel_efd`는 0(stdin) 함정 회피 위해 -1 seed.

---

## 9. 관측성 (`src/backup_manager.c`)
- `PRINT_LOG_WARN` 매크로 신규(현재 INFO/ERROR만). `print_log` static 버퍼에 **mutex**(비스레드안전 해소), `fprintf(fp,"%s",log_buffer)`(포맷스트링 버그 해소). 로그 = `$CUBRID/log/cubrid_backup.log`.
- **`LOG_MESSAGE_MAX_SIZE`≈1074B < PATH_MAX(4096)** → spool 경로는 **별도 로그 라인**(한 줄에 몰아넣으면 잘림).
- 로깅: 시작 시 적용 설정값, mem→disk spill 시작/종료, **WAIT enter/exit**(backpressure 시점=LOG_CS 지연 신호), degrade(fallocate/ENOSPC), 종료 요약(high-water/wait 누적/총바이트). hot-path per-push 로그 금지(WAIT enter/exit만).

---

## 10. 테스트 (`testcases/`)
- **tc05 (slow consumer 3-tier)**: read 사이 `usleep`. **tier는 conf의 작은 mem/disk 크기로 구동**(read 버퍼 크기 아님). MEM→SPILL→WAIT 후 restore+`cmp`. `[OK]/[NOK]`를 `*_result`에.
- **tc06 (DML 비간섭) — opt-in 별도 스테이지**: broker 기동 + 자체 대용량 DB 프로비저닝(archive 다수, `remove_archive=false`) + **CCI**(`$CUBRID/include/cas_cci.h`, `libcascci.so`, `cci_set_autocommit(TRUE)`+`clock_gettime`) auto-commit commit-latency probe. P_base/P_fix/P_off 비교 + `ER_LOG_BACKUP_CS_ENTER/_EXIT`(기본 notification) 상관. **기본 run_test.sh 흐름과 분리**(하드코딩 100M DB는 LOG_CS 재현 불가, broker 미기동).
- **conf 파싱**: buffer 키는 backupdb 명령줄에 안 실려 command-line grep 불가 → **`cubrid_backup.log`/init 리턴으로 검증**. fifo_size 1MB clamp 확인.
- **회귀/호환**: tc01+restore+cmp가 완충 ON에서 바이트 동일; **rollback 케이스(`buffer_memory_size=0`)**로 구 동작 증명; tc02 순서오류 −1 보존(drain은 begin 성공 후 시작·end에서 정리).
- **하네스 함정**: 신규 파일 **git add 선행**(`build.sh`의 `git clean -ffdx`); `$CUBRID/log/*` 와이프 전 로그 스냅샷.

---

## 11. 빌드 (`src/CMakeLists.txt`)
- `find_package(Threads REQUIRED); target_link_libraries(cubridbackupapi PRIVATE Threads::Threads)` (현재 pthread를 로더에 의존 — 내부 스레드 추가되므로 명시 링크).
- `add_compile_definitions(_GNU_SOURCE)` (헤더 #define 불가: libc 헤더가 먼저 include됨). `F_SETPIPE_SZ`/`fallocate`/`eventfd`에 필요.
- drain 코드를 새 .c로 분리 시 `CUBRID_BACKUP_API_SRCS`에 추가(소스 명시 리스트). 공개헤더(`cubrid_backup_api.h`) 불변. `VERSION` 1.0→1.1.

---

## 12. 구현 순서
1. **인프라**: CMake(Threads/_GNU_SOURCE), `PRINT_LOG_WARN`, print_log 안전화. (저위험, 런타임 무변화)
2. **파싱/검증/구조체**: §5 + 헤더 필드 + 기본값 seed + 검증(post-io_size).
3. **수명 뼈대**: §8 init/teardown + `buffering_enabled`(구 경로 유지).
4. **Phase 1 메모리 tier**: §4·§6 + F_SETPIPE_SZ + §2 동시성. → **tc01 회귀 + tc05(mem-only WAIT) 통과**.
5. **Phase 2 디스크 tier**: §7 spool + fallocate. → tc05 spill.
6. **tc06 opt-in** + 문서/샘플 conf 갱신.

---

## 13. 검증 요약 (멀티에이전트, 64 포인트)
설계대로 구현 시 터지는 **blocker**(요약): cond_wait 중 backup_mutex 보유 deadlock(§2.2), Tier-3 WAIT join-전 broadcast 누락 hang(§2.2), cancel이 `EXIT`(정상)로 위장→truncated 성공오인(§2.4), drain SIGCHLD 미차단 EOF hang(§2.3), O_NONBLOCK EAGAIN 오처리/busy-spin(§4.1), 기본값 미seed로 ON→OFF(§5), 정규식 미확장 시 경로 silent-skip(§5), 정적핸들 mutex/cond init 위치·링 리셋·teardown 3사이트(§8). DESIGN-WRONG: WINDOWS 가드 없음/`256MB` 이미 파싱됨/validate_dir free-space 없음/검증 타이밍/키 5개. verify 108 CONFIRMED / 11 PLAUSIBLE / 1 REFUTED.

## 14. 열린 리스크 & 최적화
- **리스크**: R1 SIGCHLD 근본 취약성(범위 밖), R2 eventfd fd 누수(teardown 체크), R4 EOF 재확인 유한 상한, R5 statvfs 멀티프로세스 race(WARN only), R6 tc06 flaky(상대비교/opt-in).
- **채택 최적화**: eventfd(즉시 cancel·no busy-spin), SPSC 링 직접 read(복사 1회↓·락 최소), F_SETPIPE_SZ=1MB(backupdb 청크 정합·syscall↓), WAIT 로그만, 링 크기 io_size 배수 정렬.
- **향후 검토(미채택)**: SPSC 락프리 atomic, `splice(fifo→disk_fd)` 제로카피, watermark 히스테리시스.

**Languages:** 한국어(이 문서) · [English](README.en.md)

# cubrid-backup-api

- [1. 개요](#1-개요)
- [2. 공개 API의 역할](#2-공개-api의-역할)
- [3. 빌드 및 라이브러리 적용 방법](#3-빌드-및-라이브러리-적용-방법)
- [4. 구조체와 함수](#4-구조체와-함수)
- [5. 예제](#5-예제)
- [6. 설정 파라미터](#6-설정-파라미터)
- [7. 예제 프로그램](#7-예제-프로그램)
- [부록: 저장소 구조](#부록-저장소-구조)

---

## 1. 개요

`cubrid-backup-api`는 타사 백업 솔루션이 **CUBRID 백업을 API 호출로 주고받도록** 해주는 C 라이브러리입니다. `cubrid backupdb`를 직접 실행해 산출된 파일을 찾아 옮기는 대신, 함수 호출로 백업 스트림을 넘겨받아 테이프·오브젝트 스토리지·네트워크 등 원하는 곳으로 바로 보낼 수 있습니다.

방향은 두 개입니다.

**백업 — 스트림 가져오기**

1. 라이브러리가 FIFO(이름 있는 파이프)를 만들고 `cubrid backupdb -D <fifo>`를 자식 프로세스로 실행합니다.
2. `backupdb`가 백업 이미지를 그 파이프에 씁니다.
3. 애플리케이션이 `cubrid_backup_read()`로 바이트를 받아 원하는 곳에 보냅니다.

넘겨받는 바이트는 `backupdb`가 쓴 바이트 그대로입니다. 따라서 이 API로 받아 저장한 내용은 `cubrid backupdb`로 직접 만든 백업 이미지와 동일합니다.

**복원 — 백업 이미지 다시 만들기**

보관해 둔 바이트를 `cubrid_restore_write()`로 넘기면, CUBRID가 기대하는 이름과 위치의 백업 볼륨 파일로 되살려 줍니다.

```
<backup_file_path>/<DB이름>_bk<레벨>v000
```

> **이름과 달리 데이터베이스를 복구해 주지는 않습니다.** `cubrid_restore_*`가 하는 일은 백업 볼륨 파일을 디스크에 되살려 놓는 것까지이고, 실제 복구는 그다음에 실행하는 `cubrid restoredb`가 수행합니다.
>
> 백업할 때 애플리케이션이 바이트를 테이프나 원격 스토리지로 보냈다면 로컬에는 백업 파일이 남아 있지 않습니다. 반면 `cubrid restoredb`는 로컬 파일을 요구합니다. 그래서 보관해 둔 바이트를 파일 형태로 되살리는 이 단계가 필요합니다.

### 필수 파일

| 파일 | 설명 |
|---|---|
| `cubrid_backup_api.h` | 공개 헤더 |
| `libcubridbackupapi.so` | 공유 라이브러리 |
| `$CUBRID/conf/cubrid_backup.conf` | 동작 설정 파일 (선택, [6장](#6-설정-파라미터)) |

`$CUBRID`는 CUBRID 설치 디렉토리를 가리키는 환경변수입니다.

### 사용 환경과 제약

- **Linux/POSIX 전용**입니다. Windows 지원 분기가 없습니다.
- **동시에 진행 중인 작업은 백업 1건, 복원 1건까지이며, 백업과 복원을 같이 진행할 수 없습니다.** 순차적으로는 같은 프로세스에서 `begin`~`end` 사이클을 여러 번 반복할 수 있습니다([5.3](#53-증분-백업-체인-복원) 참고).
- FIFO 경로에 PID가 들어가지 않으므로, 한 호스트에서 **같은 DB·같은 레벨을 두 프로세스가 동시에 백업하면 충돌**합니다.

---

## 2. 공개 API의 역할

| 함수 | 역할 |
|---|---|
| `cubrid_backup_initialize()` | 라이브러리 사용 시작. 설정 파일을 읽고 내부 상태를 준비합니다. |
| `cubrid_backup_begin()` | 백업 1건 시작. `backupdb` 자식 프로세스를 띄우고 `cubrid_backup_read()`에 필요한 핸들을 구성합니다. |
| `cubrid_backup_read()` | 백업 스트림에서 데이터를 읽습니다. |
| `cubrid_backup_end()` | 백업 종료 또는 취소. 핸들과 자원을 정리합니다. |
| `cubrid_restore_begin()` | 복원 1건 시작. 출력할 백업 볼륨 파일을 만들고 핸들을 돌려줍니다. |
| `cubrid_restore_write()` | 보관해 둔 백업 데이터를 그 파일에 씁니다. |
| `cubrid_restore_end()` | 복원 종료. 파일을 닫고 핸들을 정리합니다. |
| `cubrid_backup_finalize()` | 라이브러리 사용 종료. `cubrid_backup_initialize()`와 짝입니다. |

`initialize()`와 `finalize()`는 백업·복원 공용입니다.

**백업 호출 순서**

```
cubrid_backup_initialize()
        │
        ├── cubrid_backup_begin()
        │        ├── cubrid_backup_read()   ← 0을 받을 때까지 반복
        │        └── cubrid_backup_end()
        │
        └── cubrid_backup_finalize()
```

**복원 호출 순서**

```
cubrid_backup_initialize()
        │
        ├── cubrid_restore_begin()          ← 레벨 수만큼 반복 가능
        │        ├── cubrid_restore_write()   ← 보관한 순서대로 반복
        │        └── cubrid_restore_end()
        │
        └── cubrid_backup_finalize()
```

---

## 3. 빌드 및 라이브러리 적용 방법

### 라이브러리 빌드

```sh
./build.sh            # 64비트 release (RelWithDebInfo)
./build.sh -m debug   # debug 빌드
```

산출물은 다음 위치에 생성되고, 같은 내용을 묶은 `.tar.gz`가 `build_x86_64_<mode>/`에 함께 만들어집니다.

```
build_x86_64_<mode>/_install/cubrid-backup-api/
    ├── cubrid_backup_api.h
    ├── libcubridbackupapi.so            → libcubridbackupapi.so.<major>.<minor> (심볼릭 링크)
    └── libcubridbackupapi.so.<major>.<minor>    실제 라이브러리 파일
```

> `build.sh`는 시작할 때 `git clean -ffdx`를 실행합니다. **새로 만든 파일은 `git add` 하지 않으면 빌드 과정에서 지워집니다.**

### 실행 전 준비

라이브러리는 내부적으로 `$CUBRID/bin/cubrid`를 자식 프로세스로 실행합니다. 따라서 애플리케이션 **실행 시점에** 다음이 갖춰져 있어야 합니다.

- **`$CUBRID` 환경변수** — CUBRID 설치 디렉토리. 라이브러리가 실행할 `cubrid` 바이너리와 설정 파일·로그 파일 위치를 모두 이 값으로 찾습니다. 설정되어 있지 않으면 `cubrid_backup_initialize()`가 실패합니다.
- **대상 데이터베이스** — `db_name`으로 지정할 데이터베이스가 이미 만들어져 있어야 합니다.
- **서버 기동 상태** — 온라인 백업(`sa_mode=false`, 기본)은 대상 데이터베이스 서버가 떠 있어야 합니다. 오프라인 백업(`sa_mode=true`)은 서버가 내려가 있어야 합니다.

### 애플리케이션 컴파일 및 링크

헤더와 라이브러리를 애플리케이션이 참조할 수 있는 위치에 두고 링크합니다. `pthread`가 함께 필요합니다.

```sh
gcc -o backup_sample backup_sample.c -I<헤더경로> -L<라이브러리경로> -lcubridbackupapi -lpthread
```

> **소스 파일을 `-l` 옵션보다 앞에 두어야 합니다.** GNU ld는 심볼을 한 번에 훑기 때문에, 라이브러리를 먼저 적으면 정의되지 않은 심볼(undefined symbol) 오류로 링크가 실패합니다.

Makefile로 쓰는 경우:

```make
CC      = gcc
CFLAGS  = -I./include
LDFLAGS = -L./lib -lcubridbackupapi -lpthread

backup_sample: backup_sample.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
```

### 실행

동적 라이브러리를 찾을 수 있어야 합니다.

```sh
LD_LIBRARY_PATH=./lib ./backup_sample
```

상시 적용하려면 `/etc/ld.so.conf.d/`에 경로를 등록하고 `ldconfig`를 실행하거나, 애플리케이션 패키지에 `.so`를 포함하고 `RPATH`를 지정합니다.

### 통합 시 주의 — 호출 프로세스에 남는 변경

백업을 시작하면 라이브러리가 **호출한 프로세스 자체에** 다음 두 가지를 적용하고, 백업이 끝난 뒤에도 되돌리지 않습니다.

- **프로세스 그룹 변경** — 자식 프로세스를 그룹 단위로 정리하기 위해 호출 프로세스를 프로세스 그룹 리더로 만듭니다(`setpgid`). 애플리케이션이나 그 상위 관리자가 프로세스 그룹을 기준으로 시그널을 보내는 구조라면 영향을 받을 수 있습니다.
- **`SIGCHLD` 핸들러 교체** — 자식 `cubrid` 프로세스의 종료를 감지하기 위해 라이브러리 자체 핸들러를 설치합니다. 애플리케이션이 자기 자식 프로세스를 직접 관리하고 있다면 **기존 `SIGCHLD` 핸들러가 덮어써집니다.**

자식 프로세스를 직접 관리하는 애플리케이션이라면, 백업을 별도 프로세스로 분리해 실행하는 방식을 검토하세요.

---

## 4. 구조체와 함수

### 4.1 구조체와 타입

#### CUBRID_BACKUP_INFO

백업 요청 정보를 담는 구조체입니다.

```c
typedef struct cubrid_backup_info CUBRID_BACKUP_INFO;
struct cubrid_backup_info
{
    int         backup_level;
    int         remove_archive;
    int         sa_mode;
    int         no_check;
    int         compress;
    const char* db_name;
};
```

| 멤버 | 값 | 설명 |
|---|---|---|
| `backup_level` | `0` / `1` / `2` | `0` = 전체 백업, `1` = 1차 증분, `2` = 2차 증분 |
| `remove_archive` | `-1` / `0` / `1` | 백업 후 더 필요 없는 아카이브 로그 삭제 여부. `0` = 삭제 안 함, `1` = 삭제 |
| `sa_mode` | `-1` / `0` / `1` | 백업 실행 모드. `0` = 온라인(client/server), `1` = 오프라인(standalone) |
| `no_check` | `-1` / `0` / `1` | 백업 데이터 일관성 검사. `0` = 검사, `1` = 검사 생략 |
| `compress` | `-1` / `0` / `1` | 백업 데이터 압축. `0` = 무압축, `1` = 압축(LZ4) |
| `db_name` | 문자열 | 백업할 데이터베이스 이름 |

> **`-1`은 "설정 파일 값을 따른다"는 뜻입니다.** `remove_archive` · `sa_mode` · `no_check` · `compress` 네 멤버가 `-1`을 지원하며, 이때 `cubrid_backup.conf`의 같은 이름 항목이 적용됩니다. `0` 또는 `1`을 주면 설정 파일보다 우선합니다.
>
> **구조체는 멤버 전체를 반드시 초기화해서 넘기세요.** 초기화하지 않으면 대부분 `cubrid_backup_begin()`이 실패하지만 그것이 보장되지는 않습니다. 네 멤버의 쓰레기값이 우연히 유효 범위(`-1`~`1`) 안에 들면 검증을 통과해 **의도하지 않은 옵션으로 백업이 진행**되고, `db_name`이 유효하지 않은 포인터이면 **크래시가 발생할 수 있습니다.**

#### CUBRID_RESTORE_INFO

복원 요청 정보를 담는 구조체입니다.

```c
typedef struct cubrid_restore_info CUBRID_RESTORE_INFO;
struct cubrid_restore_info
{
    RESTORE_TYPE restore_type;
    int          backup_level;
    const char*  up_to_date;
    const char*  backup_file_path;
    const char*  db_name;
};
```

| 멤버 | 값 | 설명 |
|---|---|---|
| `restore_type` | `RESTORE_TO_FILE` | 복원 방식. 아래 `RESTORE_TYPE` 참고 |
| `backup_level` | `0` / `1` / `2` | 되돌릴 백업 데이터의 레벨 |
| `up_to_date` | `NULL` | 시점 지정. 아직 지원되지 않습니다. |
| `backup_file_path` | 문자열 | 백업 볼륨 파일을 만들 **디렉토리**. 미리 존재해야 합니다. |
| `db_name` | 문자열 | 복원 대상 데이터베이스 이름 |

#### RESTORE_TYPE

복원 방식을 지정하는 열거형입니다.

```c
typedef enum restore_type RESTORE_TYPE;
enum restore_type
{
    RESTORE_TO_DB,
    RESTORE_TO_FILE
};
```

| 값 | 설명 |
|---|---|
| `RESTORE_TO_DB` | 데이터베이스로 직접 복원. 아직 지원되지 않습니다. |
| `RESTORE_TO_FILE` | 백업 데이터를 백업 볼륨 파일로 씁니다. |

### 4.2 함수

#### cubrid_backup_initialize()

라이브러리 사용을 시작합니다. 다른 함수보다 먼저, 프로세스에서 한 번 호출합니다. 이 시점에 설정 파일을 읽고 값을 검증합니다.

```c
int cubrid_backup_initialize (void);
```

| 반환 | 의미 |
|---|---|
| `0` | 성공 |
| `-1` | 실패 |

**주의** — 설정 파일에 **인식할 수 없는 항목이 하나라도 있으면 실패합니다.** 항목 이름 오타가 가장 흔한 실패 원인입니다. 파일 자체가 없는 경우는 실패가 아니며 전부 기본값으로 동작합니다.

#### cubrid_backup_begin()

백업 1건을 시작합니다. `cubrid backupdb` 자식 프로세스가 이 시점에 실행됩니다.

```c
int cubrid_backup_begin (CUBRID_BACKUP_INFO* backup_info, void** backup_handle);
```

| 매개변수 | 입출력 | 설명 |
|---|---|---|
| `backup_info` | in | 백업 요청 정보. [CUBRID_BACKUP_INFO](#cubrid_backup_info) 참고 |
| `backup_handle` | out | 이후 `cubrid_backup_read()` · `cubrid_backup_end()`에 넘길 핸들 |

| 반환 | 의미 |
|---|---|
| `0` | 성공 |
| `-1` | 실패 |

**주의** — 진행 중인 백업이 있으면 실패합니다. 버퍼 관련 자원 확보에 실패한 경우에는 **실패하지 않고** 기능을 낮춰 진행합니다([6.4](#64-설정이-적용됐는지-확인) 참고).

#### cubrid_backup_read()

백업 스트림에서 데이터를 읽습니다. `0`을 받을 때까지 반복 호출합니다.

```c
int cubrid_backup_read (void* backup_handle, void* buffer,
                        unsigned int buffer_size, unsigned int* data_len);
```

| 매개변수 | 입출력 | 설명 |
|---|---|---|
| `backup_handle` | in | `cubrid_backup_begin()`에서 받은 핸들 |
| `buffer` | out | 백업 데이터를 담을 사용자 버퍼 |
| `buffer_size` | in | 버퍼 크기 |
| `data_len` | out | 버퍼에 실제로 담긴 바이트 수 |

| 반환 | 의미 |
|---|---|
| `1` | 성공. 남은 데이터가 있으므로 다시 호출해야 합니다. |
| `0` | 성공. 백업이 끝났습니다. 이때 `data_len`은 `0`입니다. |
| `-1` | 실패 |

**주의** — `-1`을 무시하면 **잘린 백업 이미지를 정상으로 착각**하게 됩니다. 반드시 확인하세요.

#### cubrid_backup_end()

백업을 끝내고 핸들과 자원을 정리합니다.

```c
int cubrid_backup_end (void* backup_handle);
```

| 매개변수 | 입출력 | 설명 |
|---|---|---|
| `backup_handle` | in | `cubrid_backup_begin()`에서 받은 핸들 |

| 반환 | 의미 |
|---|---|
| `0` | 성공 |
| `-1` | 실패 |

**주의** — 스트림을 끝까지 읽지 않은 상태에서 호출하면 **취소**로 처리됩니다. 이 경우 그때까지 받은 데이터는 완전한 백업 이미지가 아닙니다.

취소로 처리되어도 자원 정리 자체가 정상이면 이 함수는 **`0`을 반환합니다.** 즉 이 함수의 반환값으로는 백업이 완전한지 알 수 없습니다. 백업이 끝까지 완료되었는지는 **`cubrid_backup_read()`가 `0`을 반환했는지**로 판단하세요.

#### cubrid_restore_begin()

복원 1건을 시작합니다. 이 시점에 `<backup_file_path>/<db_name>_bk<backup_level>v000` 파일이 생성됩니다.

```c
int cubrid_restore_begin (CUBRID_RESTORE_INFO* restore_info, void** restore_handle);
```

| 매개변수 | 입출력 | 설명 |
|---|---|---|
| `restore_info` | in | 복원 요청 정보. [CUBRID_RESTORE_INFO](#cubrid_restore_info) 참고 |
| `restore_handle` | out | 이후 `cubrid_restore_write()` · `cubrid_restore_end()`에 넘길 핸들 |

| 반환 | 의미 |
|---|---|
| `0` | 성공 |
| `-1` | 실패 |

**주의** — 이 함수는 백업 볼륨 파일을 만들 뿐 데이터베이스를 복구하지 않습니다. 실제 복구는 파일이 준비된 뒤 `cubrid restoredb`로 수행합니다.

같은 경로에 파일이 이미 있으면 **내용을 비우고 새로 씁니다.** `backup_file_path` 디렉토리는 미리 존재해야 하며, 만들어지는 파일 권한은 `0600`입니다. 진행 중인 백업이 있으면 실패합니다.

#### cubrid_restore_write()

보관해 둔 백업 데이터를 백업 볼륨 파일에 씁니다.

```c
int cubrid_restore_write (void* restore_handle, int backup_level,
                          void* buffer, unsigned int data_len);
```

| 매개변수 | 입출력 | 설명 |
|---|---|---|
| `restore_handle` | in | `cubrid_restore_begin()`에서 받은 핸들 |
| `backup_level` | in | 데이터의 백업 레벨. **`CUBRID_RESTORE_INFO.backup_level`과 같아야 합니다.** |
| `buffer` | in | 백업 데이터가 담긴 사용자 버퍼 |
| `data_len` | in | 버퍼에 담긴 바이트 수 |

| 반환 | 의미 |
|---|---|
| `0` | 성공 |
| `-1` | 실패 |

**주의** — `backup_level`은 출력 파일을 고르는 인자가 **아니라** 실수 방지용 확인입니다. 핸들의 레벨과 다르면 그 호출이 실패합니다. 따라서 **핸들 하나가 레벨 하나, 파일 하나**이며, 전체+증분 체인을 되돌리려면 레벨마다 `begin`~`end` 사이클을 따로 돌려야 합니다. 데이터는 **백업할 때 읽은 순서 그대로** 넘겨야 합니다.

#### cubrid_restore_end()

복원을 끝내고 파일을 닫습니다.

```c
int cubrid_restore_end (void* restore_handle);
```

| 매개변수 | 입출력 | 설명 |
|---|---|---|
| `restore_handle` | in | `cubrid_restore_begin()`에서 받은 핸들 |

| 반환 | 의미 |
|---|---|
| `0` | 성공 |
| `-1` | 실패 |

#### cubrid_backup_finalize()

라이브러리 사용을 종료합니다. `cubrid_backup_initialize()`와 짝으로, 프로세스에서 한 번 호출합니다. 복원만 사용한 경우에도 이 함수를 호출합니다.

```c
int cubrid_backup_finalize (void);
```

| 반환 | 의미 |
|---|---|
| `0` | 성공 |
| `-1` | 실패 |

---

## 5. 예제

### 5.1 전체 백업

```c
#include <stdio.h>
#include "cubrid_backup_api.h"

int main (void)
{
    CUBRID_BACKUP_INFO info;
    void *handle = NULL;
    char  buf[4096];
    unsigned int len = 0;
    int   rc;
    FILE *fp;

    info.backup_level   = 0;      /* 전체 백업 */
    info.remove_archive = -1;     /* -1 = cubrid_backup.conf 값을 따른다 */
    info.sa_mode        = -1;
    info.no_check       = -1;
    info.compress       = -1;
    info.db_name        = "demodb";

    fp = fopen ("demodb_bk0v000", "wb");
    if (fp == NULL)                                 { return 1; }
    if (cubrid_backup_initialize () == -1)          { return 1; }
    if (cubrid_backup_begin (&info, &handle) == -1) { return 1; }

    do
    {
        rc = cubrid_backup_read (handle, buf, sizeof (buf), &len);

        if (rc == -1)                             /* 실패는 반드시 확인 */
        {
            cubrid_backup_end (handle);           /* 실패해도 정리는 해야 한다 */
            cubrid_backup_finalize ();
            fclose (fp);
            return 1;
        }

        if (len > 0)  { fwrite (buf, 1, len, fp); }
    } while (rc != 0);                            /* 0 = 백업 완료 */

    fclose (fp);
    if (cubrid_backup_end (handle) == -1)   { return 1; }
    if (cubrid_backup_finalize () == -1)    { return 1; }

    return 0;
}
```

실사용에서는 `fwrite` 자리에 테이프·오브젝트 스토리지 전송이 들어갑니다. 위 예제는 확인하기 쉽도록 로컬 파일에 저장한 것입니다.

> **오류가 나도 `cubrid_backup_end()`까지 호출해야 합니다.** 같은 프로세스에서 백업을 반복하는 상주 프로그램이라면, `end()`를 건너뛴 핸들 때문에 이후 `cubrid_backup_begin()`이 계속 실패할 수 있습니다.
>
> **이 예제는 버퍼 설정 없이 동작합니다.** 버퍼 관련 항목은 기본값이 꺼짐이므로, 백업 데이터를 받아가는 쪽이 느릴 수 있는 환경이라면 [6장](#6-설정-파라미터)에서 버퍼를 켜는 설정을 함께 적용하세요.

### 5.2 복원

보관해 둔 바이트를 백업 볼륨 파일로 되살립니다.

```c
#include <stdio.h>
#include <sys/stat.h>
#include "cubrid_backup_api.h"

int main (void)
{
    CUBRID_RESTORE_INFO info;
    void *handle = NULL;
    char  buf[4096];
    size_t len;
    FILE *src;

    info.restore_type     = RESTORE_TO_FILE;
    info.backup_level     = 0;
    info.up_to_date       = NULL;              /* 미지원 */
    info.backup_file_path = "./restore_dir";   /* 미리 존재해야 함 */
    info.db_name          = "demodb";

    mkdir ("./restore_dir", S_IRWXU);

    src = fopen ("demodb_bk0v000", "rb");      /* 보관해 둔 백업 데이터 */
    if (src == NULL)                                  { return 1; }
    if (cubrid_backup_initialize () == -1)            { return 1; }
    if (cubrid_restore_begin (&info, &handle) == -1)  { return 1; }

    while ((len = fread (buf, 1, sizeof (buf), src)) > 0)
    {
        /* 두 번째 인자는 info.backup_level 과 같아야 한다 */
        if (cubrid_restore_write (handle, 0, buf, len) == -1)
        {
            cubrid_restore_end (handle);
            cubrid_backup_finalize ();
            fclose (src);
            return 1;
        }
    }

    fclose (src);
    if (cubrid_restore_end (handle) == -1)  { return 1; }
    if (cubrid_backup_finalize () == -1)    { return 1; }

    return 0;
}
```

실행하면 `./restore_dir/demodb_bk0v000`이 만들어집니다. 실제 데이터베이스 복구는 그다음 단계입니다.

```sh
cubrid restoredb -B ./restore_dir -l 0 demodb
```

> `-B`로 백업 볼륨이 있는 디렉토리를, `-l`로 레벨을 지정합니다. 이 명령은 대상 데이터베이스의 내용을 덮어씁니다.

### 5.3 증분 백업 체인 복원

레벨마다 핸들이 따로이므로 `begin`~`end`를 레벨 수만큼 반복합니다.

```c
#include <stdio.h>
#include <sys/stat.h>
#include "cubrid_backup_api.h"

/* 레벨 하나를 되돌린다. 성공 0, 실패 -1 */
static int restore_one_level (int level, const char *src_path)
{
    CUBRID_RESTORE_INFO info;
    void  *handle = NULL;
    char   buf[4096];
    size_t len;
    FILE  *src;

    src = fopen (src_path, "rb");
    if (src == NULL) { return -1; }

    info.restore_type     = RESTORE_TO_FILE;
    info.backup_level     = level;
    info.up_to_date       = NULL;
    info.backup_file_path = "./restore_dir";
    info.db_name          = "demodb";

    if (cubrid_restore_begin (&info, &handle) == -1)
    {
        fclose (src);
        return -1;
    }

    while ((len = fread (buf, 1, sizeof (buf), src)) > 0)
    {
        /* 두 번째 인자는 info.backup_level 과 같아야 한다 */
        if (cubrid_restore_write (handle, level, buf, len) == -1)
        {
            cubrid_restore_end (handle);
            fclose (src);
            return -1;
        }
    }

    fclose (src);
    return cubrid_restore_end (handle);
}

int main (void)
{
    mkdir ("./restore_dir", S_IRWXU);

    if (cubrid_backup_initialize () == -1) { return 1; }

    if (restore_one_level (0, "tape/demodb_bk0v000") == -1) { return 1; }
    if (restore_one_level (1, "tape/demodb_bk1v000") == -1) { return 1; }
    if (restore_one_level (2, "tape/demodb_bk2v000") == -1) { return 1; }

    if (cubrid_backup_finalize () == -1) { return 1; }

    return 0;
}
```

세 파일이 모두 준비되면 `restoredb`에 최종 레벨을 지정합니다.

```sh
cubrid restoredb -B ./restore_dir -l 2 demodb
```

---

## 6. 설정 파라미터

설정 파일은 `$CUBRID/conf/cubrid_backup.conf`이고 `[backup]` 섹션에 작성합니다. 파일이 없으면 모든 항목이 기본값으로 동작합니다. 크기는 `64KB`, `256MB`, `1GB` 형식으로 씁니다.

> **인식할 수 없는 항목이 있으면 무시되지 않고 `cubrid_backup_initialize()`가 실패합니다.** 항목 이름을 정확히 쓰세요.
>
> 빌드는 설정 파일을 설치하지 않습니다. 직접 만들어 두세요. `testcases/cubrid_backup.conf`에 예시가 있습니다.
>
> 이 파일은 `[restore]` 섹션도 문법상 인식하지만(`partial_recovery`, `use_database_location_path`), 현재 어떤 동작에도 반영되지 않는 미사용 항목입니다. 사용하지 마세요.

### 6.1 파라미터 목록

| 항목 | 기본값 | 무엇을 위해 존재하는가 |
|---|---|---|
| `remove_archive` | `false` | 백업이 끝난 뒤 더 필요 없는 아카이브 로그를 정리해 **로그 디스크 사용량을 억제**하기 위한 항목입니다. |
| `sa_mode` | `false` | 서버를 내린 상태에서 백업해야 할 때(오프라인 백업) 사용합니다. `true`면 standalone 모드로 실행됩니다. |
| `no_check` | `false` | 백업 데이터 일관성 검사를 생략해 **백업 시간을 줄이기** 위한 항목입니다. 검사를 건너뛰는 만큼 이상 감지 시점이 늦어집니다. |
| `thread_count` | `0` | `backupdb`가 사용할 병렬 스레드 수입니다. **백업 소요 시간과 서버 부하의 균형**을 잡기 위해 사용합니다. `0`이면 서버가 결정합니다. |
| `compress` | `false` | 백업 데이터를 LZ4로 압축해 **전송량과 보관 용량을 줄이기** 위한 항목입니다. |
| `except_active_log` | `false` | 활성 로그를 백업에서 제외합니다. |
| `sleep_msecs` | `0` | 백업 진행 중 의도적으로 지연을 넣어 **운영 중 서버 부하를 낮추기** 위한 항목입니다. |
| `fifo_size` | `64KB` | 라이브러리와 `backupdb` 사이 **파이프 크기**입니다. 한 번에 더 큰 덩어리를 받기 위해 사용합니다. `[64KB, 1MB]` 범위로 제한됩니다. 버퍼를 쓰지 않을 때도 적용됩니다. |
| `buffer_memory_size` **(버퍼)** | `0` | **버퍼를 켜고 그 크기를 정하는 항목입니다.** `0`이면 버퍼를 쓰지 않고 애플리케이션이 파이프를 직접 읽습니다. |
| `buffer_disk_limit` **(버퍼)** | `0` | 메모리가 다 찼을 때 **버퍼가 디스크까지 확장될 최대 크기**입니다. `0`이면 메모리만 사용합니다. |
| `buffer_disk_path` **(버퍼)** | *(빈 값)* | 버퍼가 디스크로 확장될 때 **파일을 만들 디렉토리**입니다. `buffer_disk_limit`이 `0`보다 크면 존재하고 쓰기 가능해야 합니다. |
| `buffer_disk_keep_spool` **(버퍼)** | `false` | 진단용 항목입니다. `true`면 버퍼가 사용한 디스크 파일을 지우지 않고 남깁니다. |

**(버퍼)** 로 표시한 4개 항목은 백업 데이터를 담아두는 버퍼를 위한 것입니다. 기본값이 모두 꺼짐이고 함께 맞물려 동작하므로, 사용하려면 [6.2](#62-버퍼-파라미터를-쓸-때-알아둘-점)를 먼저 읽으세요.

`remove_archive` · `sa_mode` · `no_check` · `compress`는 `CUBRID_BACKUP_INFO`의 같은 이름 멤버가 `-1`일 때만 적용됩니다. 멤버에 `0`이나 `1`을 주면 설정 파일보다 우선합니다. 나머지 항목은 API 구조체에 대응 멤버가 없으므로 항상 설정 파일 값이 적용됩니다.

`thread_count`는 온라인 백업(`sa_mode=false`)에서 의미가 있습니다. standalone 모드에서는 서버가 병렬도를 1로 고정하므로 값을 주어도 반영되지 않습니다.

> ⚠️ **`compress` 동작이 이번 버전에서 바뀌었습니다.** `backupdb` 11.3 이상은 압축 옵션을 주지 않으면 **기본적으로 LZ4 압축**을 합니다. 그래서 이전에는 `compress=false`인데도 압축된 이미지가 나왔습니다. 이제는 무압축을 명시적으로 전달하므로 `compress=false`가 실제로 무압축을 뜻합니다.
> **기본 설정에서 백업 이미지 형식이 LZ4 → 무압축으로 바뀝니다.** 백업 크기와 소요 시간이 달라지므로, 기존 이미지 크기를 기준으로 용량을 산정해 두었다면 다시 확인하세요. 압축을 유지하려면 `compress=true`로 명시합니다.

### 6.2 버퍼 파라미터를 쓸 때 알아둘 점

여기서 다루는 항목은 `buffer_memory_size` · `buffer_disk_limit` · `buffer_disk_path` · `buffer_disk_keep_spool` 네 개입니다.

파이프는 애플리케이션이 `cubrid_backup_read()`를 호출할 때만 비워집니다. 그래서 백업 데이터를 받아가는 쪽이 느리면 파이프가 가득 차고, `backupdb`가 그 상태로 대기하면서 서버의 다른 작업까지 밀릴 수 있습니다. 버퍼 관련 항목은 **파이프를 애플리케이션 대신 계속 비워 두어 이 영향을 끊기 위한** 것입니다. 전용 스레드가 파이프에서 데이터를 빼내 버퍼에 쌓아두고, 애플리케이션은 버퍼에서 읽어갑니다. **기본값은 꺼짐이므로 필요할 때만 켜세요.**

**디스크 확장을 쓸 때**

- **`buffer_disk_limit`은 `buffer_memory_size`가 `0`보다 클 때만 의미가 있습니다.** 메모리 버퍼 없이 디스크만 지정하면 `cubrid_backup_initialize()`가 실패합니다.
- **공간은 백업이 시작될 때 미리 확보합니다.** `cubrid_backup_initialize()` 시점에 여유 공간이 `buffer_disk_limit`보다 적으면 경고만 남기고 통과합니다. 이후 백업 시작 시점에 실제로 확보가 실패하면, 그때 실패로 처리하지 않고 메모리만 사용하도록 낮춥니다.
- **`tmpfs` 같은 RAM 기반 파일시스템을 지정하면 그 공간이 디스크가 아니라 RAM을 소모합니다.**
- **`buffer_disk_keep_spool=true`는 상시 켜두지 마세요.** 남는 파일의 크기는 실제로 사용한 양이 아니라 `buffer_disk_limit` 전체입니다. 파일명에 PID가 들어가므로 백업을 새 프로세스로 돌릴 때마다 파일이 하나씩 늘어나고, 이를 지우는 코드는 없습니다. 내용은 백업 데이터 그대로입니다(권한 `0600`). 조사가 끝나면 끄고 남은 파일을 삭제하세요.

**크기를 정하는 기준**

- `buffer_memory_size` — 버퍼로 견디려는 지연 시간을 기준으로 잡습니다. 대략 `(견디려는 지연 시간) × (받아가는 쪽의 처리량)`이며, 이 메모리는 백업이 끝날 때까지 잡혀 있습니다. **최솟값 제약이 있습니다.** 라이브러리 작업 디렉토리가 있는 파일시스템에서 산출한 I/O 단위(보통 32KB) 이상이어야 하고, 이 값은 호스트마다 다를 수 있어 **어떤 서버에서 통과한 설정이 다른 서버에서 거부될 수 있습니다.**
- `buffer_disk_limit` — 메모리만으로 부족한 분량을 기준으로 잡습니다. 실제로 얼마나 필요한지는 [6.4](#64-설정이-적용됐는지-확인)의 `disk_high_water` 값으로 확인할 수 있습니다.

### 6.3 설정 예시

**1. 버퍼를 사용하지 않는 경우**

설정 파일이 없거나 `buffer_memory_size`를 지정하지 않으면 됩니다. 애플리케이션이 파이프를 직접 읽습니다.

**2. 메모리 버퍼만 사용하는 경우**

```ini
[backup]
fifo_size=1MB
buffer_memory_size=64MB
```

**3. 메모리 + 디스크 버퍼를 사용하는 경우**

```ini
[backup]
fifo_size=1MB
buffer_memory_size=64MB
buffer_disk_limit=256MB
buffer_disk_path=/var/tmp/cubrid_backup_spool
buffer_disk_keep_spool=false
```

### 6.4 설정이 적용됐는지 확인

설정을 잘못 잡아도 백업은 성공하므로, **버퍼가 실제로 켜졌는지는 로그로 확인해야 합니다.** 로그 파일은 `$CUBRID/log/cubrid_backup.log`입니다.

`cubrid_backup_begin()`은 **버퍼 자원 확보에 실패해도 실패하지 않습니다.** 메모리 할당이나 디스크 파일 생성이 안 되면 경고만 남기고 기능을 낮춰 진행합니다. 백업 이미지는 정상이지만 버퍼는 꺼진 상태입니다.

백업이 `cubrid_backup_end()`까지 정상 종료하면 다음 줄이 기록됩니다.

```
tiered buffer summary: log_phase=1 spilled=1 mem_high_water=67108864 disk_high_water=104857600 wait_count=12 lookahead=3 bytes_total=8927412224
```

| 항목 | 의미 |
|---|---|
| 이 줄이 **없다** | 버퍼가 꺼진 채 동작했습니다. 설정을 다시 확인하세요. |
| `spilled` | `1`이면 메모리가 넘쳐 디스크 확장을 실제로 사용했습니다. |
| `mem_high_water` | 메모리 버퍼가 가장 많이 찼던 크기(바이트). `buffer_memory_size`에 근접하면 여유가 없다는 뜻입니다. |
| `disk_high_water` | 디스크 확장이 가장 많이 찼던 크기(바이트). `buffer_disk_limit`을 정하는 실측 근거입니다. |
| `wait_count` | 받아가는 쪽이 느려 전용 스레드가 기다린 횟수입니다. 버퍼 크기를 늘릴 근거가 됩니다. |
| `bytes_total` | 애플리케이션에 넘긴 총 바이트 수. 저장한 데이터 크기와 일치해야 합니다. |
| `log_phase` · `lookahead` | 내부 최적화 상태를 나타내는 진단값입니다. 해석하지 않아도 됩니다. |

### 6.5 자주 겪는 문제

| 증상 | 원인과 해결 |
|---|---|
| `cubrid_backup_initialize()`가 `-1` | 설정 파일에 인식할 수 없는 항목이 있습니다(오타). 또는 `buffer_memory_size`가 최솟값보다 작거나, `buffer_disk_limit`만 지정하고 `buffer_memory_size`를 `0`으로 두었습니다. |
| `cubrid_backup_begin()`이 `-1` | `CUBRID_BACKUP_INFO`를 초기화하지 않았을 가능성이 큽니다. 네 멤버(`remove_archive`/`sa_mode`/`no_check`/`compress`)를 명시하세요. 또는 이미 진행 중인 백업이 있습니다. |
| `cubrid_restore_write()`가 `-1` | 두 번째 인자가 `CUBRID_RESTORE_INFO.backup_level`과 다릅니다. |
| 백업은 되는데 `tiered buffer summary:`가 없다 | 버퍼가 꺼진 상태입니다. `buffer_memory_size`가 `0`이거나 자원 부족으로 낮아졌습니다. 로그의 경고 줄을 확인하세요. |
| 백업 크기가 갑자기 커졌다 | `compress` 동작 변경 때문입니다([6.1](#61-파라미터-목록) 참고). 압축을 원하면 `compress=true`로 명시하세요. |
| 두 백업 파일을 바이트 비교했는데 다르다 | 정상입니다. 백업 스트림에는 실행 시각처럼 매번 달라지는 값이 들어갑니다. 두 백업이 같은지 보려면 각각 복원해 데이터 내용을 비교하세요. |

---

## 7. 예제 프로그램

`testcases/`에 동작하는 예제가 들어 있습니다. 4장에서 설명한 구조체와 함수를 실제로 사용하는 코드입니다.

| 파일 | 내용 |
|---|---|
| `backup_tc01.c` | 전체·증분 백업. 5.1 예제의 바탕이 된 코드입니다. |
| `backup_tc02.c` ~ `backup_tc04.c` | 인자 조합과 예외 상황 |
| `backup_tc05.c` | 느린 소비자를 흉내내 버퍼가 메모리 → 디스크로 넘어가는 경로를 유발한 뒤 복원 결과를 비교 |
| `restore_tc01.c` ~ `restore_tc03.c` | 복원. 5.2 예제의 바탕이 된 코드입니다. |
| `parser_ut.c` | 내부 단위 테스트 (서버 불필요) |
| `conf_test.sh` | `cubrid_backup.conf`의 각 항목이 실제 `backupdb` 호출 인자에 반영되는지 확인 (`run_test.sh`가 실행) |

### 빌드와 실행

```sh
bash testcases/run_test.sh
```

API와 예제 프로그램을 빌드하고, 100MB 크기의 `testdb`를 만들어 백업·복원 시나리오를 순서대로 실행합니다. 수 분 안에 끝나고 `ALL PASSED` 또는 `FAILED TEST SUMMARY`를 출력합니다. `expect`가 설치되어 있어야 합니다.

> ⚠️ **실험용 CUBRID 설치에서만 실행하세요.** 이 스크립트는 `cubrid service stop`으로 **그 설치의 모든 데이터베이스를 정지**시키고, `$CUBRID/conf/cubrid_backup.conf`를 삭제하고, `$CUBRID/log/`를 비우고, `testdb`를 삭제합니다. 공용 개발 서버에서 실행하면 다른 사용자의 설정과 로그가 사라집니다.

개별 프로그램 실행 방법은 각 프로그램의 `usage`에 들어 있습니다.

```sh
./backup_tc01  demodb 0 ./backup_dir/demodb_bk0v000
./restore_tc01 demodb 0 ./backup_dir/demodb_bk0v000 0 ./restore_dir
```

### 실환경 검증 스위트

```sh
CUBRID=/path/to/CUBRID bash testcases/stress/run_stress.sh [quick|standard|full]
```

11개 스위트(`s01`~`s11`)가 설정 값 조합, 오류 주입, 레벨별·복원 정확성, 대용량 내구, 커밋 지연 측정 등을 수행합니다.

> ⚠️ **실험용 CUBRID 설치에서만 실행하세요.** 이 스위트도 데이터베이스를 만들고 삭제하며 설정 파일과 로그를 변경합니다. 대용량 스위트는 디스크를 수백 GB 사용할 수 있습니다. 스위트별 내용은 [`testcases/stress/README.md`](testcases/stress/README.md)를 참고하세요.

---

## 부록: 저장소 구조

```
src/               API 구현 + 헤더
testcases/         예제 프로그램 + run_test.sh + 예시 conf
testcases/stress/  실환경 검증 스위트 (s01~s11)
build.sh           패키지 빌드 스크립트
VERSION            패키지 버전
README.md          이 문서(한국어)
README.en.md       영문판
```

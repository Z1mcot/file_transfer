Работай с уже существующим проектом `file_transfer`.

Перед началом внимательно изучи текущий код, архитектуру, `README.md`, `docs/PROTOCOL.md`, тесты и `CLAUDE.md`.

Текущая реализация уже выполняет базовую передачу файлов, проверяет целостность, использует staging `.part`, атомарную публикацию `.hex`, умеет принимать несколько клиентов и имеет таймауты. Не переписывай работающий код без необходимости.

Текущая проблема/задача:

1. Сервер ограничивает одновременно обслуживаемые подключения примерно 32 worker threads.
2. Один TCP connection сейчас обслуживается отдельным системным потоком.
3. Хотелось бы убрать искусственную зависимость «одно подключение = один thread».
4. Нужна возможность из одного запуска клиента отправлять несколько файлов одновременно.

Главная задача: провести архитектурный аудит существующей реализации и определить, как аккуратно добавить event-driven concurrency без разрушения текущего протокола, надежности и тестов.

---

# 1. Сначала проведи аудит

Не начинай сразу переписывать код.

Сначала найди конкретные места, которые сейчас ограничивают масштабирование:

* где создаются worker threads;
* где задается лимит 32;
* какие операции выполняются блокирующе;
* где socket переводится/не переводится в non-blocking;
* где выполняются `recv`, `send`, `read`, `write`;
* где реализованы таймауты;
* как устроено завершение сервера;
* какие структуры состояния существуют на одно соединение;
* насколько текущий `ITransport` совместим с event-driven моделью;
* предполагает ли существующий protocol строго «один файл = одно соединение»;
* какие части storage и transfer можно оставить без изменений.

Отдельно проверь, не является ли текущая многопоточность реальной архитектурной необходимостью.

Не предполагай заранее, что `epoll` автоматически лучше.

Сравни минимум:

```text
1. poll()
2. epoll()
3. select()
4. один thread на connection
5. thread pool + blocking sockets
```

Оцени их именно для данного проекта:

* Linux-only;
* C++20;
* нет сторонних библиотек;
* потенциально много одновременных передач;
* потоковая передача больших файлов;
* существующий протокол;
* требования к простоте тестового задания.

После анализа выбери один вариант и объясни в design note:

```text
почему выбран;
что он улучшает;
что усложняет;
какие ограничения остаются.
```

Для Linux-проекта при отсутствии веской причины предпочтительным кандидатом является `epoll`, но окончательное решение принимай по фактическому коду.

---

# 2. Целевая архитектура

Если аудит подтверждает целесообразность event-driven подхода, перестрой сервер так, чтобы количество одновременно передаваемых файлов не зависело от количества worker threads.

Целевая модель:

```text
                    +----------------------+
                    |      EventLoop       |
                    |       epoll          |
                    +----------+-----------+
                               |
              +----------------+----------------+
              |                |                |
              v                v                v
          connection A    connection B    connection C
              |                |                |
          transfer state   transfer state   transfer state
```

Идея:

* один основной event loop;
* non-blocking listening socket;
* non-blocking client sockets;
* отдельное состояние передачи для каждого connection;
* event loop реагирует на readiness;
* чтение и запись выполняются только когда socket готов;
* никаких блокирующих `recv()`/`send()` внутри event loop;
* одна передача не должна блокировать остальные.

Не нужно создавать thread на каждый connection.

Не создавай сложную систему из десятков потоков только ради дисковых операций, если это не требуется.

---

# 3. Важный момент: файлы и disk I/O

Проверь отдельно, не получится ли ситуация:

```text
epoll event loop
    |
    +--> recv()
    |
    +--> blocking write() to disk
```

Если обычная запись на диск может блокировать event loop на длительное время, это нужно учитывать.

Не нужно автоматически добавлять thread pool.

Сначала выясни, действительно ли disk I/O представляет практическую проблему для данного тестового задания и выбранной модели.

Допустимые решения:

### Вариант A

Оставить обычные файловые операции, если нагрузка приемлема и это явно документировано.

### Вариант B

Вынести потенциально блокирующие disk operations в небольшой bounded worker pool.

### Вариант C

Использовать другую архитектуру, если она лучше соответствует проекту.

Не добавляй thread pool только потому, что «так надежнее».

Главный event loop все равно не должен блокироваться сетевыми операциями.

---

# 4. Новый EventLoop должен быть отдельным слоем

Не размазывай `epoll_*` по `transfer`, `protocol` и `storage`.

Нужен отдельный компонент.

Например:

```cpp
class EventLoop;
class EventSource;
```

или другой разумный API.

Концептуально:

```text
Application
    |
    v
Server
    |
    v
EventLoop
    |
    +---- Listener
    |
    +---- ConnectionState #1
    |
    +---- ConnectionState #2
    |
    +---- ConnectionState #3
```

`protocol` и `transfer` не должны знать о `epoll`.

Транспортный слой также не должен превращаться в набор глобальных вызовов `epoll`.

Сделай четкую границу:

```text
EventLoop
    -> readiness events

Transport
    -> byte stream

Protocol
    -> frames

Transfer
    -> file transfer state machine
```

---

# 5. Передача должна стать state machine

Для event-driven модели нельзя писать код вида:

```cpp
receive_header();
receive_all_data();
receive_finish();
```

если эти функции потенциально блокируют.

Нужна state machine передачи.

Например концептуально:

```text
AwaitHello
    |
    v
ReceivingData
    |
    v
AwaitFinish
    |
    v
Validating
    |
    v
Publishing
    |
    v
SendingResult
    |
    v
Completed
```

Для каждого connection должно существовать собственное состояние:

```text
socket
protocol parser state
transfer state
bytes received
expected bytes
current sequence
CRC state
staging file
output metadata
write progress
last activity timestamp
```

Состояние одного клиента не должно смешиваться с состоянием другого.

---

# 6. Partial read / partial write

После перехода на non-blocking sockets особенно внимательно проверь:

```text
recv() == -1 && errno == EAGAIN/EWOULDBLOCK
send() == -1 && errno == EAGAIN/EWOULDBLOCK
```

Это не ошибка передачи.

Это означает:

```text
сейчас продолжать нельзя, дождаться следующего события
```

Для каждого соединения может существовать:

```cpp
read buffer
write queue
```

или более эффективная структура.

Особенно важно, чтобы `send()` не выполнялся в цикле до полного завершения буфера.

Правильная модель:

```text
EPOLLIN
    -> read available bytes
    -> advance parser

EPOLLOUT
    -> write as much as possible
    -> keep remaining bytes
    -> remove EPOLLOUT when output queue is empty
```

Не оставляй `EPOLLOUT` постоянно включенным, когда отправлять нечего.

---

# 7. Таймауты

В текущей реализации уже есть timeout для неактивных TCP connections.

После перехода на event loop сохрани это поведение без polling loop с коротким интервалом.

Не делай:

```cpp
while (running) {
    epoll_wait(..., 100);
    check_all_connections();
}
```

если это используется только ради таймаутов.

Предпочтительные варианты:

* `timerfd`;
* timeout самого `epoll_wait()` с корректным ближайшим deadline;
* другая event-driven схема.

Для Linux-проекта разумным вариантом является `timerfd`, если он действительно упрощает архитектуру.

Главное:

* нет busy loop;
* timeout не требует обхода всех sockets каждую миллисекунду;
* idle connection корректно закрывается;
* активное соединение не закрывается ошибочно.

---

# 8. Graceful shutdown

Сохрани текущую семантику:

```text
SIGINT
SIGTERM
```

Но адаптируй ее к EventLoop.

Signal handler не должен делать сложную работу.

Event loop должен получить событие:

```text
shutdown requested
```

после чего:

1. перестает принимать новые connections;
2. закрывает listener;
3. обрабатывает/отменяет активные transfers согласно выбранной политике;
4. корректно закрывает sockets;
5. очищает `.part`;
6. освобождает epoll resources;
7. завершает процесс.

Не допускай deadlock из-за shutdown.

---

# 9. Убрать искусственный лимит 32

Текущий лимит вида:

```text
maximum 32 worker threads
```

не должен оставаться архитектурным ограничением event-driven сервера.

После перехода на EventLoop:

* не должно быть `32 connections` как жесткого лимита только потому, что создано 32 threads;
* практический предел должен определяться доступными file descriptors и ресурсами ОС;
* программа не должна заранее резервировать один системный thread на каждый connection.

Если понадобится собственный защитный limit, он должен быть осознанным, документированным и не имитировать старую модель `thread-per-connection`.

Обязательно объясни в README, от чего после изменения зависит масштабирование.

---

# 10. Очень важная проверка: существующий protocol

Сначала выясни, как текущий protocol связан с моделью:

```text
one connection = one file
```

Не ломай эту связь без необходимости.

Для поддержки нескольких файлов одновременно есть два архитектурных варианта.

## Вариант A. Один connection на каждый файл

Например:

```bash
./file_transfer -c file1.bin file2.bin file3.bin
```

Клиентский процесс:

```text
                  +--> connection(file1)
                  |
client process ---+--> connection(file2)
                  |
                  +--> connection(file3)
```

Все connections обслуживает один client EventLoop.

Плюсы:

* минимальные изменения протокола;
* хорошо соответствует существующей модели;
* простая изоляция ошибок;
* server уже умеет принимать отдельные файлы;
* один упавший файл не ломает остальные.

Это предпочтительный вариант, если текущий protocol действительно рассчитан на `one connection = one file`.

## Вариант B. Один connection содержит несколько файлов

Например:

```text
connection
    file1
    file2
    file3
```

Это потребует полноценного multiplexing protocol.

Не выбирай этот вариант без серьезной причины.

Он существенно усложняет:

* protocol;
* state machine;
* ошибки;
* CRC;
* cancellation;
* partial messages;
* server storage state.

Для данного тестового задания не стоит усложнять протокол только ради нескольких файлов, если вариант A дает ту же пользовательскую возможность.

---

# 11. Новый CLI для нескольких файлов

Сохрани полную обратную совместимость:

```bash
./file_transfer -c ./file.bin
```

должно работать точно так же, как сейчас.

Расширь:

```bash
./file_transfer -c file1.bin file2.bin file3.bin
```

Все файлы должны отправляться одновременно из одного client process.

Параметры подключения также должны поддерживаться:

```bash
./file_transfer -c file1.bin file2.bin file3.bin --host 192.0.2.10 --port 5000
```

или более чистым эквивалентным форматом, если текущий parser устроен иначе.

При выборе синтаксиса не сломай существующий CLI.

Обязательно обнови:

```text
README.md
--help
CLI tests
integration tests
```

---

# 12. Поведение клиента при нескольких файлах

Один client process должен:

1. проверить все входные пути;
2. определить, какие файлы нельзя открыть;
3. создать отдельный connection/state на каждый корректный файл;
4. одновременно передавать их через event loop;
5. независимо отслеживать результат каждого файла.

Например:

```text
file1 -> SUCCESS
file2 -> CRC ERROR
file3 -> CONNECTION ERROR
file4 -> SUCCESS
```

Ошибка одного файла не должна автоматически отменять остальные.

В конце процесс должен завершаться с понятным aggregate exit status.

Например:

```text
0 -> все файлы успешно переданы
non-zero -> хотя бы один файл завершился ошибкой
```

Но обязательно выведи результат по каждому файлу.

Пример:

```text
[CLIENT] file1.bin: transfer started
[CLIENT] file2.bin: transfer started
[CLIENT] file3.bin: transfer started

[CLIENT] file2.bin: transfer failed: connection reset
[CLIENT] file1.bin: transfer completed successfully
[CLIENT] file3.bin: transfer completed successfully

[CLIENT] 2/3 files transferred successfully
```

---

# 13. Ограничение количества файлов у клиента

Не вводи искусственный маленький лимит вроде:

```text
32 files
```

только потому, что раньше было 32 threads.

Но не создавай бесконечное количество sockets одновременно.

Проанализируй:

* `RLIMIT_NOFILE`;
* количество открытых descriptors;
* размер queue;
* объем client-side memory.

Если нужен разумный limit, сделай его configurable и обоснуй.

При этом:

```bash
./file_transfer -c file1 file2 ... file100
```

не должен падать только потому, что старый код был рассчитан на 32 worker threads.

Можно использовать ограниченный concurrency window, например:

```text
N active transfers
remaining files queued
```

но это должно быть частью осознанной архитектуры, а не случайным hardcoded `32`.

---

# 14. EventLoop клиента и сервера

Рассмотри возможность использовать общий event loop abstraction и для:

```text
server
client
```

Но не форсируй это, если из-за общей абстракции код становится хуже.

Хороший результат может выглядеть так:

```text
EventLoop
   |
   +-- ServerApplication
   |
   +-- ClientApplication
```

а может быть:

```text
ServerEventLoop
ClientEventLoop
```

Выбирай по фактической структуре кода.

Главное: network readiness logic должна быть переиспользуемой, а application-specific transfer state не должен смешиваться с ней.

---

# 15. Storage

Существующую надежную storage-логику не ломай.

Должны сохраниться:

* `.part`;
* уникальное имя;
* CRC;
* проверка размера;
* `fsync`;
* atomic publish;
* защита от collision;
* cleanup при failure.

После перехода к event loop несколько connections должны безопасно работать со storage одновременно.

Проверь race conditions при:

```text
100 clients
100 staging directories
100 final files
```

---

# 16. Тесты concurrency

Существующий integration suite расширь.

## Сервер

Минимум:

```text
8 clients
32 clients
64 clients
128 clients
```

64 и 128 особенно важны: они должны демонстрировать, что сервер больше не зависит от лимита 32 worker threads.

Не нужно обязательно создавать огромные файлы.

Для concurrency-теста можно использовать небольшие файлы.

Проверить:

* все подключения обслужены;
* сервер не падает;
* содержимое всех файлов совпадает;
* нет зависаний;
* после завершения не остается `.part`;
* event loop продолжает работать после большого burst.

---

# 17. Тест клиента с несколькими файлами

Например:

```bash
./file_transfer -c file1.bin file2.bin file3.bin file4.bin
```

Проверить, что:

* все четыре передачи идут одновременно;
* сервер получает четыре файла;
* каждый `.hex` соответствует своему input file;
* одна искусственно сломанная передача не отменяет остальные;
* client exit code корректен;
* сервер продолжает принимать следующие connections.

---

# 18. Тест больших очередей

Проверь:

```text
100+ input files
```

при разумном размере каждого файла.

Если выбран concurrency window:

```text
active N
queued M
```

докажи тестом, что после завершения одного transfer следующий автоматически запускается.

---

# 19. Тест медленных клиентов

Это очень важно для event loop.

Создай test transport/client, который искусственно отправляет данные небольшими порциями с задержками.

Проверить:

```text
slow client
+
normal client
```

Normal client не должен ждать завершения slow client.

Именно этот тест должен показать, что:

```text
connection A != blocking connection B
```

---

# 20. Тест partial write

Для event-driven сервера обязательно иметь тест на ситуацию:

```text
send()
=> writes only N bytes
```

Остаток должен быть отправлен позже через `EPOLLOUT`.

Не допускается потеря конца сообщения.

---

# 21. Тест timeout

Проверить:

```text
client connects
sends part of frame
stops sending
```

После timeout:

* connection закрывается;
* `.part` удаляется;
* event loop продолжает работать;
* другой клиент успешно передает файл.

---

# 22. Проверка CPU

При полностью idle сервере:

```text
no clients
```

не должно быть busy loop.

После перехода на `epoll` это должно быть видно и архитектурно, и по поведению.

Не добавляй бессмысленные wakeups.

Если используется `timerfd`, он должен срабатывать только когда нужен timeout, а не непрерывно.

---

# 23. Проверка корректности EventLoop

Особое внимание:

* stale file descriptors;
* closed fd still registered in epoll;
* fd reuse;
* EPOLLHUP;
* EPOLLERR;
* EOF;
* удаление connection state;
* lifetime объектов;
* iterator invalidation;
* race между shutdown и worker completion;
* повторное закрытие fd;
* случайное использование уже освобожденного state.

При удалении connection из event loop должна быть четкая ownership-модель.

Документируй:

```text
who owns fd
who owns ConnectionState
who removes epoll registration
who closes fd
```

---

# 24. Не делать event loop ради event loop

Если после анализа окажется, что текущая задача плохо подходит для полного перехода на event-driven модель, не делай бессмысленную переработку.

В этом случае:

1. объясни, почему;
2. покажи, какое ограничение реально останется;
3. предложи минимальное изменение.

Но если event-driven модель явно подходит, доведи ее до рабочего состояния, а не оставляй демонстрационный skeleton.

---

# 25. Документация

Добавь отдельный документ:

```text
docs/CONCURRENCY.md
```

В нем опиши:

1. текущую модель до изменения;
2. новую модель;
3. почему выбран `epoll`/другой механизм;
4. state machine;
5. ownership;
6. timeout mechanism;
7. shutdown;
8. масштабирование;
9. ограничения.

В `README.md` обнови:

* описание concurrency;
* отсутствие жесткого лимита 32 connections;
* новый CLI с несколькими файлами;
* пример запуска;
* описание parallel transfers.

---

# 26. Производительность

Не пытайся делать micro-optimization до корректности.

Основные цели:

```text
1. correctness
2. no blocking network operations in event loop
3. predictable resource usage
4. concurrent progress
5. simple ownership
6. testability
7. performance
```

Не используй:

* lock-free структуры без необходимости;
* custom allocator;
* coroutine framework;
* io_uring;
* сложную actor system;
* сторонние runtime.

Если `epoll + state machine` решает задачу, этого достаточно.

---

# 27. Что должно быть в финальной архитектуре

Предпочтительный результат:

```text
                    +------------------+
                    |   Application    |
                    +--------+---------+
                             |
                    +--------v---------+
                    |    EventLoop     |
                    |      epoll       |
                    +---+----------+---+
                        |          |
             +----------+          +----------+
             v                                v
      ConnectionState                  ConnectionState
             |                                |
        Protocol/Transfer                Protocol/Transfer
             |                                |
        ITransport                      ITransport
             |                                |
          TCP socket                       TCP socket
```

А для клиента:

```text
                   Client process
                        |
                    EventLoop
                        |
        +---------------+---------------+
        |               |               |
      file1           file2           file3
        |               |               |
    connection      connection      connection
```

Протокол при этом желательно оставить:

```text
one connection = one file
```

если аудит не покажет убедительную необходимость multiplexing.

---

# 28. Обязательно проверь исходный код, а не только README

README уже говорит, что:

* worker threads ограничены 32;
* каждый accepted connection получает worker;
* TCP reads имеют inactivity timeout;
* accept loop использует `poll()`;
* server currently processes connections through worker threads;
* client currently отправляет один файл за процесс.

Не принимай README как истину.

Подтверди это по фактическому коду.

Если README расходится с реализацией, исправь README и исходный код так, чтобы они соответствовали друг другу.

---

# 29. Запрещенные упрощения

Не делай следующие вещи:

* не просто увеличивай `32 -> 128`;
* не создавай thread на каждый файл;
* не заменяй worker threads на огромный thread pool и не называй это EventLoop;
* не используй blocking sockets внутри event loop;
* не используй busy polling;
* не ломай существующий однофайловый CLI;
* не ломай существующий protocol без необходимости;
* не удаляй существующие safety checks storage;
* не удаляй CRC;
* не удаляй tests только потому, что архитектура поменялась.

---

# 30. Финальная проверка

После реализации обязательно:

1. clean build;
2. unit tests;
3. integration tests;
4. concurrency tests;
5. multi-file client tests;
6. slow-client test;
7. timeout test;
8. interrupted-client test;
9. 64+ simultaneous connections;
10. 100+ files from one client process;
11. SIGINT;
12. SIGTERM;
13. проверка отсутствия busy loop;
14. проверка descriptor leaks;
15. проверка `.part` cleanup;
16. проверка отсутствия races;
17. проверка partial send/recv;
18. проверка EOF/HUP/ERR;
19. проверка сохранения файлов рядом с executable;
20. проверка обратной совместимости:

```bash
./file_transfer -c file.bin
```

должна продолжать работать.

---

# 31. Финальный review

После тестов сделай отдельный code review именно как Linux network programmer.

Ищи:

* fd lifetime bugs;
* epoll registration bugs;
* fd reuse bugs;
* stale events;
* use-after-free;
* partial writes;
* partial frame parsing;
* incorrect EPOLLOUT handling;
* incorrect timeout calculations;
* event loop starvation;
* slow-client starvation;
* shutdown races;
* incorrect signal handling;
* descriptor leaks;
* resource exhaustion;
* accidental hardcoded connection limit;
* incorrect multi-file state;
* incorrect aggregate exit code клиента;
* protocol regressions.

Проверяй не только happy path.

---

# 32. Результат

Нужен полностью рабочий код.

Не пиши:

```text
"можно использовать epoll"
"архитектура может выглядеть так"
"это можно реализовать позже"
```

Нужно:

* реально изменить код;
* реализовать выбранную concurrency model;
* реализовать multi-file client;
* сохранить обратную совместимость;
* написать/обновить тесты;
* обновить документацию;
* собрать проект;
* прогнать тесты;
* исправить проблемы.

В финальном отчете обязательно покажи:

1. что было в старой архитектуре;
2. что изменилось;
3. почему выбран именно этот механизм;
4. как теперь масштабируются connections;
5. как реализована отправка нескольких файлов;
6. какие ограничения остались;
7. какие тесты подтверждают новую архитектуру;
8. реальные результаты тестов;
9. self-rating согласно `CLAUDE.md`;
10. итоговый статус согласно `CLAUDE.md`.

Если EventLoop не нужен или переход на него имеет серьезный недостаток, сначала зафиксируй этот вывод с доказательствами. Не меняй архитектуру только ради самого факта использования `epoll`.

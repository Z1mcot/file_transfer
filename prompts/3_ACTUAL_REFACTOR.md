Ты работаешь с существующим проектом `file_transfer`. Проведи реализацию изменений по уже выполненному архитектурному аудиту. Не возвращайся к общему проектированию и не выдавай рекомендации вместо кода: задача этого этапа — реально внести изменения в репозиторий, собрать проект, прогнать тесты и исправить найденные проблемы.

Сначала прочитай `CLAUDE.md` и соблюдай все его правила работы, включая branching/worktree, triage, тесты, critic review, self-rating и финальный статус.

Ниже зафиксированы результаты аудита и решения, которые нужно реализовать.

---

# 1. Исходное состояние

Текущий сервер:

* создает `std::thread` на каждое принятое соединение;
* имеет `maximum_active_clients = 32`;
* использует блокирующие sockets;
* `receive_file()` и `send_file()` являются линейными блокирующими драйверами;
* timeout сейчас реализован через `SO_RCVTIMEO = 30s`;
* send-timeout отсутствует;
* protocol рассчитан на `one connection = one file`;
* storage уже реализует надежную staging/publication-модель;
* текущие unit/integration tests проверяют существующую transfer/storage логику.

По результатам аудита выбрана новая модель:

```text
Linux epoll
level-triggered
один event-loop thread
non-blocking sockets
per-connection state machine
```

Эту архитектуру и нужно реализовать.

Не заменяй ее снова на `poll`, thread pool или thread-per-connection.

---

# 2. Главная цель

Нужно получить две вещи:

## Сервер

Убрать модель:

```text
1 connection = 1 system thread
```

и заменить ее на:

```text
1 event loop
N connections
N independent transfer states
```

Сервер больше не должен иметь жесткий лимит `32 connections`, вызванный количеством worker threads.

Практический предел должен определяться ресурсами ОС, прежде всего file descriptors.

## Клиент

Сохранить:

```bash
./file_transfer -c file.bin
```

и добавить:

```bash
./file_transfer -c file1.bin file2.bin file3.bin
```

Один клиентский процесс должен одновременно обслуживать несколько отдельных TCP connections.

При этом:

```text
one connection = one file
```

оставить неизменным.

Не делать multiplexing нескольких файлов внутри одного TCP connection.

Это решение выбрано намеренно, чтобы не ломать существующий wire protocol и не усложнять protocol state.

---

# 3. Главный принцип реализации

Не переписывай рабочую protocol/storage логику с нуля.

Переиспользуй уже существующие:

* frame encoding/decoding;
* protocol validation;
* CRC32;
* `IFileStore`;
* `IStagedFile`;
* filename generation;
* fsync;
* atomic publication;
* cleanup;
* существующие blocking transport tests.

Новая архитектура должна добавить event-driven orchestration вокруг этой логики.

Целевая схема:

```text
ServerApplication / ClientApplication
                |
                v
           EventLoop
        epoll + timerfd
                |
        +-------+--------+
        |                |
        v                v
 ConnectionState    ConnectionState
        |                |
        v                v
 Transfer Machine  Transfer Machine
        |                |
        v                v
   Protocol layer   Protocol layer
        |                |
        v                v
    ITransport       ITransport
        |                |
     TCP socket       TCP socket
```

Protocol и transfer не должны знать о `epoll`.

---

# 4. EventLoop

Создай отдельный reusable компонент, например:

```cpp
class EventLoop;
```

Название можешь изменить, если существующая структура проекта требует другого.

Он должен отвечать только за event-driven orchestration.

Минимальные обязанности:

* создать/управлять epoll fd;
* register fd;
* modify interest mask;
* remove fd;
* wait for events;
* dispatch events;
* корректно обрабатывать `EPOLLIN`;
* корректно обрабатывать `EPOLLOUT`;
* корректно обрабатывать `EPOLLERR`;
* корректно обрабатывать `EPOLLHUP`;
* корректно обрабатывать EOF;
* безопасно удалять dead handlers.

Не смешивай `epoll` с protocol parsing.

---

# 5. Level-triggered epoll

Используй обычный level-triggered режим.

Не используй `EPOLLET`, если для него нет сильной причины.

Причина:

* код проще;
* меньше риск зависания из-за недочитанного сокета;
* проект не требует экстремальной оптимизации event dispatch;
* тестируемость важнее последнего процента производительности.

---

# 6. Ownership и lifetime

Это критическая часть.

Для каждого connection должен существовать объект состояния, например:

```cpp
class ServerConnection;
class ClientTransfer;
```

или эквивалент.

Он должен владеть:

* socket transport;
* protocol parser state;
* transfer state;
* file state;
* CRC state;
* counters;
* output queue;
* last activity timestamp.

Ownership должен быть однозначным.

Предпочтительная модель:

```text
EventLoop
    owns unique_ptr<Handler>
Handler
    owns TcpTransport
TcpTransport
    owns fd
```

`epoll` не должен хранить fd как единственный идентификатор объекта.

Используй безопасный механизм привязки события к handler.

Допустимо:

```text
epoll_event.data.ptr -> Handler*
```

если lifetime строго контролируется.

Важно:

* handler нельзя уничтожать посреди обработки текущего `epoll_wait` batch;
* сначала помечай handler как dead;
* физически удаляй после завершения обработки batch;
* `close(fd)` должен быть только в одном понятном месте;
* не должно быть double-close;
* не должно быть use-after-free;
* fd reuse не должен приводить к обращению к старому state.

---

# 7. TCP sockets

После перехода серверных connections на EventLoop:

* listener должен работать в non-blocking режиме;
* accepted sockets должны работать в non-blocking режиме;
* client sockets должны быть non-blocking;
* `recv()`/`send()` должны корректно обрабатывать `EAGAIN`/`EWOULDBLOCK`;
* `connect()` на клиенте должен быть non-blocking.

Нельзя превращать `EAGAIN` в ошибку передачи.

Нужен четкий результат API transport:

```text
bytes read/written
would block
EOF
fatal error
```

---

# 8. Transport API

Не ломай существующий `ITransport`, если он нужен для существующих blocking-driver tests.

Добавь узкий интерфейс для non-blocking stream, например:

```cpp
class IStreamSocket {
public:
    virtual ~IStreamSocket() = default;

    virtual int fd() const noexcept = 0;

    virtual ReadResult recv_nonblocking(
        std::span<std::byte> buffer) = 0;

    virtual WriteResult send_nonblocking(
        std::span<const std::byte> buffer) = 0;

    virtual int connect_error() const = 0;

    virtual void close() noexcept = 0;
};
```

Это только пример.

Сделай API в стиле существующего проекта.

Главное:

* event loop не знает TCP internals;
* transfer не знает epoll;
* TCP adapter знает только transport mechanics.

---

# 9. Не используй блокирующий network I/O в EventLoop

Ни один handler event loop не должен выполнять:

```cpp
recv_until_complete(...)
send_until_complete(...)
read_frame_blocking(...)
```

или эквивалент.

На каждом событии разрешено прочитать/записать то, что доступно сейчас, после чего управление возвращается EventLoop.

---

# 10. Server state machine

Перепиши серверную transfer-логику в incremental state machine.

Целевые состояния:

```text
AwaitHello
    |
    v
Receiving
    |
    v
AwaitFinish
    |
    v
Validating / Publishing
    |
    v
SendingResult
    |
    v
Completed
```

При ошибке:

```text
AnyState
    |
    v
Failed
    |
    v
Closed
```

Не обязательно использовать enum с абсолютно этими именами.

Но состояния должны быть явными и легко прослеживаться.

---

# 11. ReceiveMachine

Выдели независимую от event loop transfer machine, например:

```cpp
class ReceiveMachine;
```

Она не должна знать:

* epoll;
* TCP;
* fd;
* thread;
* EventLoop.

Она должна принимать байты/frames и менять состояние передачи.

Концептуально:

```cpp
feed(std::span<const std::byte>)
```

может возвращать результат вида:

```text
need more data
protocol error
transfer failed
transfer completed
send result
```

или другой эквивалентный API.

Основная идея:

```text
network bytes
    ->
parser
    ->
transfer state
    ->
storage
```

а не:

```text
network bytes
    ->
epoll-specific transfer code
```

---

# 12. FrameParser

Если текущий protocol decoder не предназначен для частичного frame input, добавь:

```cpp
class FrameParser;
```

Он должен поддерживать:

* frame header, пришедший частями;
* payload, пришедший частями;
* несколько frames одним `recv`;
* frame, разбитый на десятки `recv`;
* EOF посреди frame;
* payload слишком большого размера;
* invalid magic;
* invalid version;
* invalid type;
* invalid payload length.

Никаких предположений:

```text
one recv = one frame
```

---

# 13. DATA processing

При получении DATA:

1. проверить sequence number;
2. проверить payload size;
3. проверить block CRC32;
4. записать bytes в staging file;
5. обновить whole-file CRC;
6. обновить received bytes;
7. проверить размер относительно HELLO metadata.

Нельзя принимать данные за пределами ожидаемого размера.

---

# 14. FINISH

При FINISH проверить:

* total bytes;
* total chunks;
* whole-file CRC32;
* полученный размер;
* CRC accumulated state;
* protocol consistency.

Только после этого разрешить publication.

Существующее надежное storage behavior сохранить:

```text
write
fsync file
atomic publish
fsync relevant directories
report result
```

Не удаляй эти проверки ради упрощения EventLoop.

---

# 15. RESULT и EPOLLOUT

RESULT может быть не отправлен одним `send()`.

Поэтому для каждого connection должна существовать write queue / pending output.

Правильная модель:

```text
prepare RESULT
    |
    v
queue bytes
    |
    v
enable EPOLLOUT
    |
    v
send some bytes
    |
    +--> all sent --> disable EPOLLOUT
    |
    +--> EAGAIN --> wait for next EPOLLOUT
```

Никогда не держи `EPOLLOUT` включенным постоянно, если queue пустая.

---

# 16. SendMachine для клиента

Клиент должен получить аналогичную event-driven state machine.

Пример:

```text
Resolving / CreatingSocket
    |
    v
Connecting
    |
    v
Hashing
    |
    v
SendingHello
    |
    v
SendingData
    |
    v
SendingFinish
    |
    v
WaitingResult
    |
    v
Completed / Failed
```

Для каждого файла свой state.

---

# 17. Client multi-file CLI

Сохрани полную обратную совместимость:

```bash
./file_transfer -c file.bin
```

Добавь:

```bash
./file_transfer -c file1.bin file2.bin file3.bin
```

Опции:

```text
--host HOST
--port PORT
--max-active N
```

Допустимый синтаксис:

```bash
./file_transfer -c file1.bin file2.bin file3.bin --host 127.0.0.1 --port 5000
```

Также поддержи `--` для явного завершения списка positional arguments:

```bash
./file_transfer -c -- file1 --weird-name.bin
```

Не меняй обязательный одnofile CLI.

---

# 18. ClientTransfer

Для каждого файла создавай независимый объект состояния:

```cpp
ClientTransfer
```

Он должен содержать:

* path;
* socket;
* protocol state;
* hashing state;
* bytes sent;
* CRC;
* output queue;
* status;
* error;
* timestamps.

Один файл не должен использовать mutable state другого.

---

# 19. Несколько файлов одновременно

Не делай:

```text
file1 -> полностью
file2 -> полностью
file3 -> полностью
```

Последовательно.

Нужно:

```text
file1 --+
file2 --+--> Client EventLoop
file3 --+
file4 --+
```

То есть несколько TCP connections должны продвигаться одновременно.

---

# 20. Ограничение client concurrency

Не используй прежний `32` как новый magic number.

Сделай параметр:

```bash
--max-active N
```

Если параметр не передан, вычисляй разумное значение на основе доступных file descriptors.

Ориентир:

```text
max_active =
min(number_of_files,
    (RLIMIT_NOFILE - safety_margin) / fd_cost_per_transfer)
```

Не копируй формулу буквально, если текущая архитектура требует другого расчета.

Главное:

* не резервировать тысячи sockets заранее;
* не падать из-за слишком большого списка файлов;
* остальные файлы помещаются в очередь;
* после завершения transfer автоматически запускается следующий.

---

# 21. Проверка входных файлов

До старта всех передач проверь входной список.

Для каждого файла:

* существует ли;
* regular file ли это;
* можно ли открыть;
* не является ли директорией.

Ошибочный файл не должен ломать остальные передачи.

Например:

```text
file1 -> OK
file2 -> directory
file3 -> OK
```

Должно привести к:

```text
file1 transfer started
file2 failed: not a regular file
file3 transfer started
```

---

# 22. Hashing большого файла

Текущая архитектура считает CRC первым проходом.

После перехода на event loop нельзя надолго блокировать event loop на огромном файле.

Не делай:

```cpp
crc32_entire_file(); // блокирует event loop на минуты
```

Используй incremental hashing.

Например:

```text
Hashing state
    |
    +-- process up to 1 MiB
    |
    +-- return to event loop
    |
    +-- continue later
```

Размер slice можешь выбрать разумно.

Это работа, а не ожидание:

* можно выполнять конечный объем CPU work за итерацию;
* затем возвращаться к event dispatch.

Но не допускай бесконечного CPU loop, если есть активная работа с network events.

---

# 23. Non-blocking connect

Client `connect()` должен быть non-blocking.

Ожидаемый сценарий:

```text
socket
    |
    v
connect()
    |
    +--> immediately connected
    |
    +--> EINPROGRESS
             |
             v
        EPOLLOUT
             |
             v
        getsockopt(SO_ERROR)
```

Не считай `EPOLLOUT` успехом без проверки `SO_ERROR`.

---

# 24. Timeout architecture

Текущий `SO_RCVTIMEO` больше не должен быть основным механизмом server connection timeout.

Используй единый event-driven timeout mechanism.

Предпочтительно:

```text
timerfd
```

с deadline по ближайшему timeout.

Нужны как минимум:

* receive inactivity timeout;
* send stall timeout.

Оба могут использовать одну структуру deadline.

Не делай:

```cpp
epoll_wait(..., 100);
check_every_connection();
```

если эта конструкция нужна только ради таймаутов.

Не используй busy polling.

Для тестов добавь:

```bash
--idle-timeout-ms N
```

или эквивалентный параметр.

По умолчанию сохрани текущее поведение около 30 секунд.

---

# 25. Timeout semantics

Сервер должен закрывать connection, если:

* клиент ничего не отправляет слишком долго;
* сервер имеет pending output, но клиент слишком долго его не принимает.

При timeout:

1. соединение закрывается;
2. `.part` очищается;
3. transfer state уничтожается;
4. event loop продолжает работу;
5. другие connections не затрагиваются.

---

# 26. SIGINT / SIGTERM

Убери старый signal waiting thread, если после перехода он больше не нужен.

Используй event-driven механизм, предпочтительно:

```text
signalfd
```

Сигналы нужно блокировать до создания event loop resources, чтобы не получить race между запуском и обработкой signal.

При shutdown:

1. перестать принимать новые connections;
2. закрыть listener;
3. отменить active transfers;
4. очистить `.part`;
5. корректно закрыть sockets;
6. удалить registrations;
7. завершить event loop.

Один event loop thread должен делать shutdown последовательным и предсказуемым.

SIGPIPE по-прежнему должен быть безопасно обработан.

---

# 27. Listener accept loop

Listener должен быть non-blocking.

На `EPOLLIN`:

```text
while accept() succeeds:
    create connection
    register it
```

Ожидаемые условия:

```text
EAGAIN/EWOULDBLOCK
```

не являются ошибкой.

Обработай:

```text
EMFILE
ENFILE
EINTR
```

корректно.

Сохрани или улучши существующий backoff для resource exhaustion.

Не допускай accept-loop busy spin.

---

# 28. File descriptor limits

После удаления server limit `32` не вводи другой искусственный limit без необходимости.

Сервер должен масштабироваться примерно до количества connections, которое позволяет:

```text
RLIMIT_NOFILE
memory
kernel socket resources
application state
```

README должен честно это описывать.

Например, если одна transfer state использует несколько descriptors, укажи это.

Опционально можно проверить `RLIMIT_NOFILE` и оставить небольшой safety margin.

---

# 29. Storage descriptors

Проверь существующий `FileStore`.

Возможно и желательно:

* держать общий output-directory fd на `FileStore`;
* не открывать его заново для каждой передачи;
* оставить private staging directory fd на transfer.

Но не меняй storage API без необходимости.

Главное:

```text
concurrent transfers
    ->
safe independent staging directories
    ->
collision-free publish
```

---

# 30. Не переносить fsync в thread pool заранее

Не добавляй bounded commit worker pool только потому, что теоретически `fsync()` может блокировать.

Сначала реализуй:

```text
inline commit
```

и измерь его влияние.

Если тесты покажут, что один `fsync` действительно блокирует event loop настолько, что нарушаются timeout/concurrency требования, тогда можно добавить bounded worker pool.

Но не усложняй первую реализацию без измерения.

---

# 31. Важный fairness rule

Один connection не должен monopolize event loop.

При обработке `EPOLLIN` не читай бесконечно гигабайты подряд.

Введи разумный budget на одну dispatch iteration.

Например:

```text
не более 256 KiB за один handler callback
```

После этого вернуть управление event loop.

Это особенно важно при сочетании:

```text
fast client + slow client
```

и большого числа одновременно готовых sockets.

---

# 32. Integration test: slow client

Добавь test scenario:

```text
slow client
normal client
```

Slow client специально передает данные небольшими порциями.

Пока slow client еще передает файл, normal client должен успешно закончить свою передачу.

Тест должен доказать отсутствие:

```text
connection A blocking connection B
```

---

# 33. Integration test: 64/128 clients

Обнови concurrency integration test.

Проверить минимум:

```text
8 clients
32 clients
64 clients
128 clients
```

64 и 128 особенно важны.

Результат должен показывать:

* нет rejection из-за старого лимита 32;
* все transfer завершены;
* все files byte-for-byte совпадают;
* сервер продолжает работать;
* после burst нет `.part`;
* после burst нет staging directories;
* нет descriptor leak.

Существующий тест:

```text
test_worker_limit_and_recovery
```

перепиши под новый контракт.

Он больше не должен ожидать:

```text
Client rejected: active connection limit reached
```

---

# 34. Integration test: много файлов через одного клиента

Добавь тест:

```text
100+ files
```

в рамках одного client process.

Проверить:

* все files queued;
* одновременно активны не более `--max-active`;
* после завершения одного transfer автоматически запускается следующий;
* все files eventually transferred;
* итоговый exit code корректен;
* output files совпадают byte-for-byte.

---

# 35. Multi-file test с одной ошибкой

Например:

```text
file1 -> success
file2 -> forced failure
file3 -> success
file4 -> success
```

Проверить:

* file2 не отменяет file1/file3/file4;
* результат каждого файла выводится отдельно;
* `.part` для failed transfer удаляется;
* server остается работоспособным;
* client возвращает non-zero exit code;
* сообщение aggregate summary корректно.

---

# 36. Partial write tests

Добавь unit tests, которые принудительно моделируют:

```text
send() writes only N bytes
```

Проверить:

* остаток остается в queue;
* EPOLLOUT включается;
* следующий callback отправляет остаток;
* после полного отправления EPOLLOUT отключается;
* RESULT/data не повреждаются.

---

# 37. FrameParser tests

Добавь тесты:

1. header за один `recv`;
2. header по одному байту;
3. header в нескольких частях;
4. payload по нескольким `recv`;
5. несколько frames одним `recv`;
6. EOF посреди header;
7. EOF посреди payload;
8. oversized payload;
9. invalid magic;
10. invalid version;
11. invalid message type.

---

# 38. Timeout tests

Добавь тест:

```text
client connects
sends only part of frame
stops
wait timeout
```

Проверить:

* connection закрыт;
* `.part` удален;
* server event loop не умер;
* следующий client успешно передает файл.

Сделай timeout коротким через:

```text
--idle-timeout-ms
```

чтобы тест не занимал 30 секунд.

---

# 39. fd leak test

Во время integration test:

1. взять количество `/proc/<pid>/fd`;
2. выполнить большой burst clients;
3. дождаться завершения;
4. снова взять количество fd.

Не ожидай обязательно абсолютно одинакового числа.

Проверь отсутствие постоянного роста.

Также проверь отсутствие незакрытых staging directory descriptors.

---

# 40. Idle CPU test

Когда сервер запущен и клиентов нет:

```text
sleep fixed interval
```

замерить `/proc/<pid>/stat`.

Подтвердить, что idle event loop не использует заметное CPU из-за polling loop.

Не делай тест чрезмерно строгим и зависящим от конкретной машины.

---

# 41. Documentation

Добавь:

```text
docs/CONCURRENCY.md
```

Содержимое:

1. старая модель;
2. проблемы старой модели;
3. новая `epoll` model;
4. level-triggered rationale;
5. handler ownership;
6. state machine;
7. EPOLLIN/EPOLLOUT;
8. timerfd;
9. signalfd;
10. shutdown;
11. fd scaling;
12. client multi-file scheduling;
13. hashing slices;
14. disk I/O choice;
15. known limitations.

Обнови README:

* больше нет server worker limit 32;
* server использует event loop;
* client умеет несколько files;
* есть `--max-active`;
* есть `--idle-timeout-ms` для тестирования;
* текущие ограничения.

---

# 42. Убрать устаревшие элементы

После миграции обязательно проверь, что больше не осталось мертвых элементов старой архитектуры:

* `maximum_active_clients = 32`;
* worker rejection;
* worker pool, если он больше не используется;
* worker join/reap логика, не нужная EventLoop;
* старые shutdown assumptions;
* старые tests, которые проверяют rejection;
* старые README statements.

Не удаляй код вслепую: сначала убедись, что он действительно стал не нужен.

---

# 43. Backward compatibility

Обязательные сценарии должны продолжить работать:

```bash
./file_transfer -s
```

```bash
./file_transfer -c ./file.bin
```

Также:

```bash
./file_transfer -c ./file.bin --host 127.0.0.1 --port 5000
```

Protocol wire format по возможности не изменяй.

Существующие `.hex` semantics не меняй.

---

# 44. Compile / warning policy

Сохрани существующие warning flags и требования C++20.

Не допускай новых warnings.

Особенно проверь:

* signed/unsigned;
* narrowing conversions;
* enum conversions;
* fd -> pointer conversions;
* `uint64_t`/`size_t`;
* `epoll_event` initialization.

---

# 45. Sanitizers

Если toolchain позволяет, добавь или используй:

```text
AddressSanitizer
UndefinedBehaviorSanitizer
ThreadSanitizer
```

где применимо.

Особенно важно прогнать:

* normal integration;
* 128-client burst;
* multi-file client;
* shutdown tests.

Если `ThreadSanitizer` не подходит из-за структуры проекта, не ломай build ради него, но зафиксируй причину.

---

# 46. Финальная архитектурная проверка

Перед завершением ответь на следующие вопросы по фактическому коду:

1. Может ли 1 slow client заблокировать остальные?
2. Может ли один connection monopolize event loop?
3. Можно ли обслужить >32 connections?
4. Есть ли hidden connection limit?
5. Может ли `send()` block event loop?
6. Может ли `recv()` block event loop?
7. Может ли timeout block event loop?
8. Может ли client hashing block event loop надолго?
9. Может ли fd reuse привести к stale handler?
10. Есть ли use-after-free после `epoll_wait`?
11. Может ли закрытый fd остаться зарегистрированным?
12. Может ли RESULT потерять хвост при partial write?
13. Может ли `.part` остаться после failed transfer?
14. Может ли ошибка одного файла остановить остальные client transfers?
15. Может ли один client process отправить 100+ files?
16. Что происходит при `EMFILE`?
17. Что происходит при `EPOLLERR`?
18. Что происходит при `EPOLLHUP`?
19. Что происходит при EOF посреди frame?
20. Что происходит при SIGINT во время передачи?

На каждый вопрос должен существовать конкретный ответ в коде.

---

# 47. Финальный testing matrix

Перед завершением прогнать:

```text
unit
integration
8 connections
32 connections
64 connections
128 connections
slow + normal client
partial write
partial read
EOF mid-frame
timeout
client kill
SIGINT
SIGTERM
100+ files from one client
multi-file with one failure
fd leak
idle CPU
```

Также выполнить:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

И любые дополнительные test lanes, которые уже существуют в проекте.

---

# 48. Финальный code review

После зеленых тестов не останавливайся.

Проведи отдельный review как Linux network programmer.

Ищи:

* use-after-free;
* stale epoll events;
* fd reuse;
* double close;
* descriptor leaks;
* event starvation;
* accidental blocking calls;
* infinite read/write loops;
* incorrect EAGAIN handling;
* incorrect EPOLLOUT management;
* timerfd bugs;
* signalfd/shutdown races;
* resource exhaustion;
* integer overflow;
* protocol regressions;
* multi-file scheduling bugs;
* partial frame bugs;
* cleanup failures;
* hidden hardcoded concurrency limits.

Проверь фактический код, а не только документацию.

---

# 49. Что считать готовым

Работа считается завершенной только если:

* сервер действительно работает через event-driven `epoll`;
* sockets действительно non-blocking;
* нет `thread-per-connection`;
* нет старого жесткого лимита 32;
* client поддерживает несколько файлов;
* transfers действительно идут одновременно;
* protocol остается совместимым;
* storage guarantees сохранены;
* tests обновлены;
* новые tests проходят;
* documentation обновлена;
* clean build проходит;
* нет новых compiler warnings;
* review не обнаружил нерешенных критических проблем.

Не выдавай псевдореализацию и не оставляй TODO вместо обязательного кода.

Если в процессе реализации обнаружится, что какое-либо решение из этого задания конфликтует с фактическим кодом, сначала исправь архитектурное противоречие в рамках уже выбранных целей, а не возвращай проект к старой thread-per-connection модели.

---

# 50. Финальный отчет

В конце отчета обязательно покажи:

```text
Triage
Architecture changes
Server concurrency
Client multi-file behavior
Protocol compatibility
Resource limits
Tests run
Test results
Problems found and fixed
Files changed
Self-rating
Final status
```

И отдельно укажи:

```text
старый лимит 32: удален / не удален
epoll: реализован / не реализован
multi-file client: реализован / не реализован
```

Статус должен соответствовать реальности и `CLAUDE.md`.

Не объявляй `DONE`, если код не собран или тесты не пройдены.
::

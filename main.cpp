#include <iostream>
#include <csignal>
#include <memory>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>

using namespace std;

#define NELEMS(arr) (sizeof(arr)/sizeof(arr[0]))

//
// Interfaces
//

template <typename Key, class Notification>
struct ObserverInterface {
    virtual void notify(Notification &notification) = 0;
    virtual Key key() const = 0;
};

template <typename Key>
struct ObservableInterface {
    typedef ObserverInterface<Key, ObservableInterface<Key>> Observer;
    virtual void addObserver(shared_ptr<Observer> &observer) = 0;
    virtual void delObserver(shared_ptr<Observer> &observer) = 0;
    virtual void delObserver(Key key) = 0;
};

struct ReaderInterface {
    virtual void read(int sock_fd) = 0;
};

struct LoggerInterface {
    virtual void write(const char *buf, int len) = 0;
};

struct HandlerInterface {
    virtual void handle(char *buf, int len) = 0;
};

//
// Implementation
//

class SocketReader : public ReaderInterface {
    public:
    explicit SocketReader(shared_ptr<HandlerInterface> &handler) :
        _handler(handler) {}
    ~SocketReader() {}
    void read(int sock_fd)
    {
        int len;
        char buf[2048];
        while ((len = ::read(sock_fd, buf, NELEMS(buf) - 1)) > 0)
            _handler->handle(buf, len);
    }
    private:
    shared_ptr<HandlerInterface> _handler;
};

class StreamHandler : public HandlerInterface {
    public:
    explicit StreamHandler(shared_ptr<LoggerInterface> &logger) :
        _logger(logger) {}
    ~StreamHandler() {}
    void handle(char *buf, int len) { _logger->write(buf, len); }
    private:
    shared_ptr<LoggerInterface> _logger;
};

class SyslogHandler : public HandlerInterface {
    public:
    explicit SyslogHandler(shared_ptr<LoggerInterface> &logger) :
        _logger(logger) {}
    ~SyslogHandler() {}
    void handle(char *buf, int len)
    {
        int start = 0;
        int end = len - 1;
        // Drop numerically coded log level and facility as we perform no
        // filtering based on this information.
        if (start < len && buf[start] == '<') {
            do ++start; while (start < len && isdigit(buf[start]));
            if (buf[start] == '>')
                ++start;
        }
        while (end > 0 && buf[end] == '\n')
            buf[end--] = '\0';
        _logger->write(buf + start, end - start);
    }
    private:
    shared_ptr<LoggerInterface> _logger;
};

class JournalHandler : public HandlerInterface {
    public:
    explicit JournalHandler(shared_ptr<LoggerInterface> &logger) :
        _logger(logger) {}
    ~JournalHandler() {}
    void handle(char *buf, int len)
    {
        for (int pos = 0; pos < len; pos++)
            if (buf[pos] == '\n')
                buf[pos] = ' ';
        _logger->write(buf, len);
    }
    private:
    shared_ptr<LoggerInterface> _logger;
};

class FileLogger : public LoggerInterface {
    public:
    explicit FileLogger(int fileno) :
        _fileno(fileno) {}
    ~FileLogger() {}
    void write(const char *buf, int len)
    {
        ::write(_fileno, "\n", 1);
        ::write(_fileno, buf, len);
    }
    private:
    int _fileno;
};

void epoll_addwatch(int epoll_fd, int sock_fd)
{
    struct epollin_event:epoll_event {
        epollin_event(int fd)
        {
            events = EPOLLIN;
            data.fd = fd;
        }
    } ev (sock_fd);
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev);
}

void fd_set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int unix_open(int type, const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        unlink(path);
        strncpy(&sa.sun_path[0], path, NELEMS(sa.sun_path));
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

int unix_accept(int stdout_fd)
{
    int fd;
    struct sockaddr_un sa;
    socklen_t slen = sizeof(sa);
    fd = accept4(stdout_fd, (struct sockaddr *)&sa, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    return fd;
}

class SocketObservable : public ObservableInterface<int> {
    public:
    SocketObservable() :
        epoll_fd(epoll_create1(EPOLL_CLOEXEC)) {}
    ~SocketObservable() {}

    void addObserver(shared_ptr<Observer> &observer)
    {
        if (observers.find(observer->key()) == observers.end())
            epoll_addwatch(epoll_fd, observer->key());
        observers[observer->key()] = observer;
    }

    void delObserver(int key)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, key, NULL);
        observers.erase(key);
    }

    void delObserver(shared_ptr<Observer> &observer)
    {
        delObserver(observer->key());
    }

    void loop() throw (runtime_error)
    {
        /* Ignore flush request for time being */
        signal(SIGUSR1, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SocketObservable::stop);
        signal(SIGTERM, SocketObservable::stop);

        struct epoll_event ev;
        while (!stopped) {
            int r = epoll_wait(epoll_fd, &ev, 1, -1);
            if (r < 0 && errno != EINTR)
                throw runtime_error("epoll_wait failed");
            if (observers.find(ev.data.fd) != observers.end())
                observers.at(ev.data.fd)->notify(*this);
        }
    }

    static void stop(int signo)
    {
        cout << "stopped signo = " << signo << endl;
        stopped = true;
    }

    private:
    int epoll_fd;
    map<int, shared_ptr<Observer>> observers;
    static bool stopped;
};
bool SocketObservable::stopped = false;

class SyslogObserver : public ObservableInterface<int>::Observer {
    public:
    explicit SyslogObserver(shared_ptr<ReaderInterface> reader) :
        sock_fd(unix_open(SOCK_DGRAM, "/run/systemd/journal/dev-log")),
        _reader(reader)
    {
        if (sock_fd < 0)
            throw runtime_error("socket failed");
        fd_set_nonblock(sock_fd);
        symlink("/run/systemd/journal/dev-log", "/dev/log");
    }

    ~SyslogObserver() { close(sock_fd); }

    void notify(ObservableInterface<int> &notification) { _reader->read(sock_fd); }

    int key() const { return sock_fd; }

    private:
    int sock_fd;
    shared_ptr<ReaderInterface> _reader;
};

class SocketObserver : public ObservableInterface<int>::Observer {
    public:
    explicit SocketObserver(shared_ptr<ReaderInterface> reader) :
        sock_fd(unix_open(SOCK_DGRAM, "/run/systemd/journal/socket")),
        _reader(reader)
    {
        if (sock_fd < 0)
            throw runtime_error("socket failed");
        fd_set_nonblock(sock_fd);
    }

    ~SocketObserver() { close(sock_fd); }

    void notify(ObservableInterface<int> &notification) { _reader->read(sock_fd); }

    int key() const { return sock_fd; }

    private:
    int sock_fd;
    shared_ptr<ReaderInterface> _reader;
};

class StreamObserver : public ObservableInterface<int>::Observer {
    public:
    explicit StreamObserver(int listen_sock, shared_ptr<ReaderInterface> reader) :
        sock_fd(unix_accept(listen_sock)),
        _reader(reader)
    {
        if (sock_fd < 0)
            throw runtime_error("accept failed");
        fd_set_nonblock(sock_fd);
     }
    ~StreamObserver() { close(sock_fd); }

    void notify(ObservableInterface<int> &notification)
    {
        _reader->read(sock_fd);
        notification.delObserver(sock_fd);
    }

    int key() const { return sock_fd; }

    private:
    int sock_fd;
    shared_ptr<ReaderInterface> _reader;
};

class StdoutObserver : public ObservableInterface<int>::Observer {
    public:
    explicit StdoutObserver(shared_ptr<ReaderInterface> reader) :
        sock_fd(unix_open(SOCK_STREAM, "/run/systemd/journal/stdout")),
        _reader(reader)
    {
        if (sock_fd < 0)
            throw runtime_error("socket failed");
        listen(sock_fd, SOMAXCONN);
    }

    ~StdoutObserver() { close(sock_fd); }

    int key() const { return sock_fd; }

    void notify(ObservableInterface<int> &notification)
    {
        shared_ptr<ObservableInterface<int>::Observer> streamObserver =
            make_shared<StreamObserver>(sock_fd, _reader);
        notification.addObserver(streamObserver);
    }

    private:
    int sock_fd;
    shared_ptr<ReaderInterface> _reader;
};

auto main()-> int
{
    shared_ptr<LoggerInterface> fileLogger = make_shared<FileLogger>(fileno(stdout));

    shared_ptr<HandlerInterface> syslogHandler = make_shared<SyslogHandler>(fileLogger);
    shared_ptr<HandlerInterface> journalHandler = make_shared<JournalHandler>(fileLogger);
    shared_ptr<HandlerInterface> streamHandler = make_shared<StreamHandler>(fileLogger);

    shared_ptr<ReaderInterface> syslogReader = make_shared<SocketReader>(syslogHandler);
    shared_ptr<ReaderInterface> journalReader = make_shared<SocketReader>(journalHandler);
    shared_ptr<ReaderInterface> streamReader = make_shared<SocketReader>(streamHandler);

    SocketObservable watcher;
    try {
        shared_ptr<SocketObservable::Observer> syslogObserver =
            make_shared<SyslogObserver>(syslogReader);
        watcher.addObserver(syslogObserver);
    } catch (runtime_error &err) {
        cerr << err.what() << endl;
    }
    try {
        shared_ptr<SocketObservable::Observer> journalObserver =
            make_shared<SocketObserver>(journalReader);
        watcher.addObserver(journalObserver);
    } catch (runtime_error &err) {
        cerr << err.what() << endl;
    }
    try {
        shared_ptr<SocketObservable::Observer> streamObserver =
            make_shared<StdoutObserver>(streamReader);
        watcher.addObserver(streamObserver);
    } catch (runtime_error &err) {
        cerr << err.what() << endl;
    }
    watcher.loop();
    return 0;
}

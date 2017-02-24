# nologd
C++ clone of lightweight non-logger

# Description
This program is a C++ clone of nologd project at github.com/lmctl/nologd

Nologd is simple utility that prints out logs from journald syslog sockets:
* /dev/log - used by POSIX's syslog(3)
* /run/systemd/journal/socket - "explicit" systemd logging (sd_journal functions)
* /run/systemd/journal/stdout - "implicit" systemd logging (stdout/stderr of service processes)

It was created as a benchmark, not real journald replacement.
